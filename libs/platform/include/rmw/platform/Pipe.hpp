#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/ipc/Frame.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Named-pipe transport for rmw::ipc messages (docs/03 §8):
// - DACL: the current user and SYSTEM only; PIPE_REJECT_REMOTE_CLIENTS;
// - the server owns the name (FILE_FLAG_FIRST_PIPE_INSTANCE): a squatter makes create() fail;
// - the client checks who serves the pipe (image path + session) and connects with
//   SECURITY_IDENTIFICATION, so a fake server cannot impersonate the user.
// Boundary (docs/06, T3): processes of the same user at medium integrity are inside it.
namespace rmw::platform
{

// One connected end. Messages are framed (ipc::encodeFrame). Move-only.
class PipeConnection
{
public:
    PipeConnection() noexcept = default;
    ~PipeConnection();
    PipeConnection(const PipeConnection&) = delete;
    PipeConnection& operator=(const PipeConnection&) = delete;
    PipeConnection(PipeConnection&& other) noexcept;
    PipeConnection& operator=(PipeConnection&& other) noexcept;

    [[nodiscard]] Result<void> send(std::string_view message, std::chrono::milliseconds timeout);
    // Timeout when nothing complete arrived in time; NotFound when the peer disconnected.
    [[nodiscard]] Result<std::string> receive(std::chrono::milliseconds timeout);

    [[nodiscard]] bool isOpen() const noexcept { return m_pipe != nullptr; }
    // Ends the reads and writes in progress (from any thread); they fail at once.
    void cancel() noexcept;
    // Process at the other end.
    [[nodiscard]] std::uint32_t peerProcessId() const noexcept { return m_peer; }
    void close() noexcept;

private:
    friend class PipeServer;
    friend Result<PipeConnection> connectPipe(const std::wstring&, std::chrono::milliseconds,
                                              const std::filesystem::path&);
    PipeConnection(void* pipe, std::uint32_t peer);

    [[nodiscard]] Result<std::size_t> transfer(bool write, void* buffer, std::size_t bytes,
                                               std::chrono::milliseconds timeout);

    // One thread may receive while another sends: each direction has its own event.
    void* m_pipe = nullptr;       // HANDLE
    void* m_event = nullptr;      // HANDLE for overlapped reads
    void* m_writeEvent = nullptr; // HANDLE for overlapped writes
    std::uint32_t m_peer = 0;
    ipc::FrameDecoder m_decoder;
};

class PipeServer
{
public:
    // Creates the first instance of `name`; AlreadyExists when another process holds it.
    [[nodiscard]] static Result<PipeServer> create(std::wstring name);

    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    PipeServer(PipeServer&& other) noexcept;
    PipeServer& operator=(PipeServer&& other) noexcept;

    // Waits for the next client; Timeout when none came (or one left before it was accepted). The
    // connection is independent of the server (each client gets its own instance). After any other
    // failure the instance is replaced, so the next accept starts clean. `cancel` (an event, may be
    // null) ends the wait at once: Cancelled.
    [[nodiscard]] Result<PipeConnection> accept(std::chrono::milliseconds timeout, void* cancel = nullptr);
    // No client can connect from now on: the name is gone once the connections handed out close too.
    // (A client finds nobody at once, instead of a server that never answers.) Accept fails after it.
    void close() noexcept;

    [[nodiscard]] const std::wstring& name() const noexcept { return m_name; }

private:
    PipeServer() noexcept = default;
    [[nodiscard]] Result<void> createInstance(bool first);
    void recycle() noexcept;

    std::wstring m_name;
    void* m_instance = nullptr;   // HANDLE waiting for a client
    void* m_event = nullptr;      // HANDLE
    void* m_descriptor = nullptr; // PSECURITY_DESCRIPTOR (LocalAlloc)
    bool m_closed = false;
};

// Connects to `name`, retrying while all instances are busy. NotFound when nobody serves it.
// AccessDenied when the server is not `expectedServerImage` in this session.
[[nodiscard]] Result<PipeConnection> connectPipe(const std::wstring& name, std::chrono::milliseconds timeout,
                                                 const std::filesystem::path& expectedServerImage);

// \\.\pipe\RicingManagerWindows-<user SID>-<session id>
[[nodiscard]] Result<std::wstring> corePipeName();

// The full path of this process's executable.
[[nodiscard]] Result<std::filesystem::path> currentExecutable();

// The full path of another process's executable (PROCESS_QUERY_LIMITED_INFORMATION).
[[nodiscard]] Result<std::filesystem::path> processImagePath(std::uint32_t pid);
// Paths compared ordinally, ignoring case.
[[nodiscard]] bool samePath(const std::filesystem::path& a, const std::filesystem::path& b);

} // namespace rmw::platform
