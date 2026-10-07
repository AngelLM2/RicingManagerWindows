#include "rmw/platform/Pipe.hpp"

#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <sddl.h>

#include <array>
#include <format>
#include <utility>
#include <vector>

namespace rmw::platform
{
namespace
{

constexpr DWORD kBufferBytes = 64 * 1024;
constexpr std::wstring_view kPipePrefix = LR"(\\.\pipe\)";

Result<std::wstring> userSid()
{
    wil::unique_handle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::unexpected(lastError("OpenProcessToken"));
    }
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return std::unexpected(lastError("GetTokenInformation"));
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wil::unique_hlocal_string text;
    if (ConvertSidToStringSidW(user->User.Sid, text.put()) == FALSE)
    {
        return std::unexpected(lastError("ConvertSidToStringSid"));
    }
    return std::wstring(text.get());
}

DWORD waitMs(std::chrono::milliseconds timeout) noexcept
{
    if (timeout.count() <= 0)
    {
        return 0;
    }
    return timeout.count() >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(timeout.count());
}

} // namespace

Result<std::filesystem::path> processImagePath(std::uint32_t pid)
{
    const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
    {
        return std::unexpected(lastError("open the pipe's peer process"));
    }
    std::array<wchar_t, 32'768> buffer{};
    auto size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) == FALSE)
    {
        return std::unexpected(lastError("QueryFullProcessImageName"));
    }
    return std::filesystem::path(std::wstring(buffer.data(), size));
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    const std::wstring& x = a.native();
    const std::wstring& y = b.native();
    return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()),
                                TRUE) == CSTR_EQUAL;
}

// ---- connection -------------------------------------------------------------------------------

PipeConnection::PipeConnection(void* pipe, std::uint32_t peer)
    : m_pipe(pipe)
    , m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_peer(peer)
{
}

PipeConnection::~PipeConnection()
{
    close();
}

PipeConnection::PipeConnection(PipeConnection&& other) noexcept
    : m_pipe(std::exchange(other.m_pipe, nullptr))
    , m_event(std::exchange(other.m_event, nullptr))
    , m_writeEvent(std::exchange(other.m_writeEvent, nullptr))
    , m_peer(std::exchange(other.m_peer, 0))
    , m_decoder(std::move(other.m_decoder))
{
}

PipeConnection& PipeConnection::operator=(PipeConnection&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_pipe = std::exchange(other.m_pipe, nullptr);
        m_event = std::exchange(other.m_event, nullptr);
        m_writeEvent = std::exchange(other.m_writeEvent, nullptr);
        m_peer = std::exchange(other.m_peer, 0);
        m_decoder = std::move(other.m_decoder);
    }
    return *this;
}

void PipeConnection::close() noexcept
{
    if (m_pipe != nullptr)
    {
        CancelIoEx(m_pipe, nullptr);
        CloseHandle(m_pipe);
        m_pipe = nullptr;
    }
    for (void** event : {&m_event, &m_writeEvent})
    {
        if (*event != nullptr)
        {
            CloseHandle(*event);
            *event = nullptr;
        }
    }
}

void PipeConnection::cancel() noexcept
{
    if (m_pipe != nullptr)
    {
        CancelIoEx(m_pipe, nullptr);
    }
}

Result<std::size_t> PipeConnection::transfer(bool write, void* buffer, std::size_t bytes,
                                             std::chrono::milliseconds timeout)
{
    // Reads and writes have events of their own: one thread may read while another writes.
    void* const event = write ? m_writeEvent : m_event;
    if (m_pipe == nullptr || event == nullptr)
    {
        return fail(ErrorCode::InvalidArgument, "the pipe is closed");
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    const auto chunk = static_cast<DWORD>(bytes > kBufferBytes ? kBufferBytes : bytes);
    DWORD done = 0;
    const BOOL started = write ? WriteFile(m_pipe, buffer, chunk, &done, &overlapped)
                               : ReadFile(m_pipe, buffer, chunk, &done, &overlapped);
    if (started == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED)
        {
            return fail(ErrorCode::NotFound, "the other side disconnected");
        }
        if (error != ERROR_IO_PENDING)
        {
            return std::unexpected(win32Error(error, write ? "write to pipe" : "read from pipe"));
        }
        if (WaitForSingleObject(event, waitMs(timeout)) == WAIT_TIMEOUT)
        {
            CancelIoEx(m_pipe, &overlapped);
            // The I/O must end before `overlapped` dies, and it may have completed just before the
            // cancel: those bytes count (dropped, the frames after them would never decode again).
            if (GetOverlappedResult(m_pipe, &overlapped, &done, TRUE) == FALSE || done == 0)
            {
                return fail(ErrorCode::Timeout,
                            write ? "timed out writing to the pipe" : "timed out waiting for a message");
            }
        }
        else if (GetOverlappedResult(m_pipe, &overlapped, &done, FALSE) == FALSE)
        {
            const DWORD finished = GetLastError();
            if (finished == ERROR_BROKEN_PIPE || finished == ERROR_PIPE_NOT_CONNECTED)
            {
                return fail(ErrorCode::NotFound, "the other side disconnected");
            }
            return std::unexpected(win32Error(finished, write ? "write to pipe" : "read from pipe"));
        }
    }
    if (!write && done == 0)
    {
        return fail(ErrorCode::NotFound, "the other side disconnected");
    }
    return static_cast<std::size_t>(done);
}

Result<void> PipeConnection::send(std::string_view message, std::chrono::milliseconds timeout)
{
    auto frame = ipc::encodeFrame(message);
    if (!frame)
    {
        return std::unexpected(std::move(frame.error()));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < frame->size())
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto wrote = transfer(true, frame->data() + sent, frame->size() - sent, left);
        if (!wrote)
        {
            return std::unexpected(std::move(wrote.error()));
        }
        sent += *wrote;
    }
    return {};
}

Result<std::string> PipeConnection::receive(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::byte> buffer(kBufferBytes);
    while (true)
    {
        if (auto message = m_decoder.next())
        {
            return std::move(*message);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            return fail(ErrorCode::Timeout, "timed out waiting for a message");
        }
        auto read = transfer(false, buffer.data(), buffer.size(), left);
        if (!read)
        {
            return std::unexpected(std::move(read.error()));
        }
        if (auto fed = m_decoder.feed(std::span(buffer).first(*read)); !fed)
        {
            close(); // a peer that breaks framing is not talked to again
            return std::unexpected(std::move(fed.error()));
        }
    }
}

// ---- server -----------------------------------------------------------------------------------

PipeServer::~PipeServer()
{
    if (m_instance != nullptr)
    {
        CancelIoEx(m_instance, nullptr);
        CloseHandle(m_instance);
    }
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
    }
    if (m_descriptor != nullptr)
    {
        LocalFree(m_descriptor);
    }
}

PipeServer::PipeServer(PipeServer&& other) noexcept
    : m_name(std::move(other.m_name))
    , m_instance(std::exchange(other.m_instance, nullptr))
    , m_event(std::exchange(other.m_event, nullptr))
    , m_descriptor(std::exchange(other.m_descriptor, nullptr))
{
}

PipeServer& PipeServer::operator=(PipeServer&& other) noexcept
{
    if (this != &other)
    {
        const PipeServer old(std::move(*this));
        m_name = std::move(other.m_name);
        m_instance = std::exchange(other.m_instance, nullptr);
        m_event = std::exchange(other.m_event, nullptr);
        m_descriptor = std::exchange(other.m_descriptor, nullptr);
    }
    return *this;
}

Result<PipeServer> PipeServer::create(std::wstring name)
{
    if (!name.starts_with(kPipePrefix))
    {
        return fail(ErrorCode::InvalidArgument, R"(pipe names start with \\.\pipe\)");
    }
    auto sid = userSid();
    if (!sid)
    {
        return std::unexpected(std::move(sid.error()));
    }
    PipeServer server;
    server.m_name = std::move(name);
    // Protected DACL: full access for this user and SYSTEM, nobody else.
    const std::wstring sddl = std::format(L"D:P(A;;GA;;;{})(A;;GA;;;SY)", *sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                             nullptr) == FALSE)
    {
        return std::unexpected(lastError("build the pipe security descriptor"));
    }
    server.m_descriptor = descriptor;
    server.m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (server.m_event == nullptr)
    {
        return std::unexpected(lastError("CreateEvent"));
    }
    if (auto created = server.createInstance(true); !created)
    {
        return std::unexpected(std::move(created.error()));
    }
    return server;
}

Result<void> PipeServer::createInstance(bool first)
{
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), m_descriptor, FALSE};
    const DWORD openMode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE pipe = CreateNamedPipeW(
        m_name.c_str(), openMode,
        PIPE_REJECT_REMOTE_CLIENTS /* | PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, all 0 */,
        PIPE_UNLIMITED_INSTANCES, kBufferBytes, kBufferBytes, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (first && (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY))
        {
            return fail(ErrorCode::AlreadyExists, "another process already serves this pipe", error);
        }
        return std::unexpected(win32Error(error, "CreateNamedPipe"));
    }
    m_instance = pipe;
    return {};
}

Result<PipeConnection> PipeServer::accept(std::chrono::milliseconds timeout)
{
    if (m_instance == nullptr)
    {
        if (auto created = createInstance(false); !created)
        {
            return std::unexpected(std::move(created.error()));
        }
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = m_event;
    ResetEvent(m_event);
    if (ConnectNamedPipe(m_instance, &overlapped) == FALSE)
    {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            DWORD ignored = 0;
            if (WaitForSingleObject(m_event, waitMs(timeout)) == WAIT_TIMEOUT)
            {
                CancelIoEx(m_instance, &overlapped);
                // A client that connected as we gave up is taken by the next accept (PIPE_CONNECTED).
                GetOverlappedResult(m_instance, &overlapped, &ignored, TRUE);
                return fail(ErrorCode::Timeout, "no client connected");
            }
            error = GetOverlappedResult(m_instance, &overlapped, &ignored, FALSE) != FALSE ? ERROR_SUCCESS
                                                                                          : GetLastError();
        }
        if (error == ERROR_NO_DATA)
        {
            // A client came and went before it was accepted: the instance is of no use any more.
            recycle();
            return fail(ErrorCode::Timeout, "a client left before it was accepted");
        }
        if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED)
        {
            // Kept, a failed instance fails every accept after it at once (a core spinning).
            recycle();
            return std::unexpected(win32Error(error, "ConnectNamedPipe"));
        }
    }

    ULONG client = 0;
    GetNamedPipeClientProcessId(m_instance, &client);
    PipeConnection connection(std::exchange(m_instance, nullptr), client);
    // The next client gets a fresh instance (created lazily on the next accept).
    return connection;
}

void PipeServer::recycle() noexcept
{
    if (m_instance == nullptr)
    {
        return;
    }
    DisconnectNamedPipe(m_instance);
    CloseHandle(m_instance);
    m_instance = nullptr; // the next accept makes a fresh one
}

// ---- client -----------------------------------------------------------------------------------

Result<PipeConnection> connectPipe(const std::wstring& name, std::chrono::milliseconds timeout,
                                   const std::filesystem::path& expectedServerImage)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (true)
    {
        // IDENTIFICATION: the server may learn who we are, never act as us.
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return fail(ErrorCode::NotFound, "nobody serves the pipe", error);
        }
        if (error != ERROR_PIPE_BUSY)
        {
            return std::unexpected(win32Error(error, "connect to pipe"));
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || WaitNamedPipeW(name.c_str(), waitMs(left)) == FALSE)
        {
            return fail(ErrorCode::Busy, "the pipe stayed busy");
        }
    }
    wil::unique_handle owned(pipe);

    ULONG serverPid = 0;
    if (GetNamedPipeServerProcessId(owned.get(), &serverPid) == FALSE)
    {
        return std::unexpected(lastError("GetNamedPipeServerProcessId"));
    }
    DWORD serverSession = 0;
    DWORD ourSession = 0;
    if (ProcessIdToSessionId(serverPid, &serverSession) == FALSE ||
        ProcessIdToSessionId(GetCurrentProcessId(), &ourSession) == FALSE || serverSession != ourSession)
    {
        return fail(ErrorCode::AccessDenied, "the pipe is served from another session");
    }
    auto image = processImagePath(serverPid);
    if (!image)
    {
        return std::unexpected(std::move(image.error()));
    }
    if (!samePath(*image, expectedServerImage))
    {
        return fail(ErrorCode::AccessDenied, std::format("the pipe is served by an unexpected program ({})",
                                                         image->filename().string()));
    }
    return PipeConnection(owned.release(), serverPid);
}

Result<std::wstring> corePipeName()
{
    auto sid = userSid();
    if (!sid)
    {
        return std::unexpected(std::move(sid.error()));
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return std::format(L"{}RicingManagerWindows-{}-{}", kPipePrefix, *sid, session);
}

Result<std::filesystem::path> currentExecutable()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return std::unexpected(lastError("GetModuleFileName"));
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        if (buffer.size() >= 32'768)
        {
            return fail(ErrorCode::LimitExceeded, "executable path too long");
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace rmw::platform
