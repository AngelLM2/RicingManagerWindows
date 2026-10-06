#include "rmw/client/CoreClient.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"

#include <windows.h>

#include <format>
#include <thread>

namespace rmw::client
{

using namespace std::chrono_literals;
using ipc::Json;

CoreClient::CoreClient(std::filesystem::path dataDirectory)
    : m_dataDirectory(std::move(dataDirectory))
{
}

CoreClient::~CoreClient() = default;
CoreClient::CoreClient(CoreClient&&) noexcept = default;
CoreClient& CoreClient::operator=(CoreClient&&) noexcept = default;

void CoreClient::disconnect() noexcept
{
    m_connection.reset();
}

Result<void> CoreClient::connect(bool startIfNeeded)
{
    m_startIfNeeded = startIfNeeded;
    auto self = platform::currentExecutable();
    auto name = platform::corePipeName();
    if (!self || !name)
    {
        return fail(ErrorCode::System, "cannot locate the core");
    }
    m_coreExe = self->parent_path() / L"rmw-core.exe";
    m_pipe = *name;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto connected = connectOrStart();
        if (!connected)
        {
            return std::unexpected(std::move(connected.error()));
        }
        auto hello = call("hello", ipc::helloParams(kVersion), 10s);
        if (hello)
        {
            if (auto compatible = ipc::checkHello(*hello); !compatible)
            {
                return compatible;
            }
            // Same protocol but another build (an update left the old core running): it would not
            // know newer commands, so it is replaced too.
            if (json::stringOr(*hello, "buildId", "") == kVersion || attempt > 0)
            {
                m_hello = std::move(*hello);
                return checkDataDirectory(m_hello);
            }
        }
        else if (hello.error().code() != ErrorCode::Unsupported)
        {
            return std::unexpected(std::move(hello.error()));
        }
        // An older core is still running (e.g. after an update): replace it.
        [[maybe_unused]] const auto stopped = call("shutdown", Json::object(), 5s);
        m_connection.reset();
        std::this_thread::sleep_for(1s);
    }
    return fail(ErrorCode::Unsupported, "the running core is from another version and did not stop");
}

Result<Json> CoreClient::call(std::string_view method, const Json& params, std::chrono::milliseconds timeout)
{
    if (!m_connection)
    {
        return fail(ErrorCode::Io, "not connected to the core");
    }
    const std::int64_t id = m_nextId++;
    if (auto sent = m_connection->send(ipc::requestText(id, method, params), 10s); !sent)
    {
        m_connection.reset();
        return std::unexpected(std::move(sent.error()));
    }
    auto reply = m_connection->receive(timeout);
    if (!reply)
    {
        m_connection.reset(); // a late reply would answer the next call
        return std::unexpected(std::move(reply.error()));
    }
    auto response = ipc::parseResponse(*reply);
    if (!response || response->id != id)
    {
        m_connection.reset();
        return fail(ErrorCode::InvalidData, "unexpected reply from the core");
    }
    if (response->error)
    {
        return std::unexpected(ipc::fromRpcError(*response->error));
    }
    return std::move(response->result);
}

// There is one core per user session, whatever folder it keeps its data in.
Result<void> CoreClient::checkDataDirectory(const Json& hello) const
{
    if (m_dataDirectory.empty())
    {
        return {};
    }
    const auto normalized = [](const std::filesystem::path& path) {
        std::error_code ec;
        auto text = std::filesystem::absolute(path, ec).lexically_normal().native();
        while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/'))
        {
            text.pop_back();
        }
        return text;
    };
    const std::filesystem::path serving(utf8To16Lossy(json::stringOr(hello, "dataDirectory", "")));
    const auto mine = normalized(m_dataDirectory);
    const auto theirs = normalized(serving);
    if (CompareStringOrdinal(mine.c_str(), static_cast<int>(mine.size()), theirs.c_str(),
                             static_cast<int>(theirs.size()), TRUE) != CSTR_EQUAL)
    {
        return fail(ErrorCode::AlreadyExists,
                    std::format("a core for another data folder is running ({}); stop it first with `rmw "
                                "shutdown`",
                                utf16To8Lossy(theirs)));
    }
    return {};
}

Result<void> CoreClient::connectOrStart()
{
    auto connection = platform::connectPipe(m_pipe, 2s, m_coreExe);
    if (!connection && connection.error().code() == ErrorCode::NotFound && m_startIfNeeded)
    {
        std::wstring arguments;
        if (!m_dataDirectory.empty())
        {
            arguments = L"--data-dir \"" + m_dataDirectory.native() + L"\"";
        }
        // Outside the caller's job: closing a terminal (or the settings) must not end the core.
        auto core = platform::ChildProcess::spawn(
            m_coreExe, arguments, {.breakawayFromJob = true, .ownJob = false, .inheritHandles = {}});
        if (!core)
        {
            return std::unexpected(std::move(core.error()).withContext("start rmw-core"));
        }
        // A core that starts after a crash first undoes the interrupted operation (it may take a
        // while); one that exits says why in its log.
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (true)
        {
            connection = platform::connectPipe(m_pipe, 2s, m_coreExe);
            if (connection || connection.error().code() != ErrorCode::NotFound ||
                std::chrono::steady_clock::now() > deadline)
            {
                break;
            }
            if (const auto exited = core->wait(0ms); exited && *exited != 0)
            {
                return fail(ErrorCode::System,
                            std::format("rmw-core stopped at start (code {}); see logs\\core.log", *exited));
            }
            std::this_thread::sleep_for(200ms);
        }
        core->detach();
    }
    if (!connection)
    {
        return std::unexpected(std::move(connection.error()).withContext("connect to rmw-core"));
    }
    m_connection = std::make_unique<platform::PipeConnection>(std::move(*connection));
    return {};
}

} // namespace rmw::client
