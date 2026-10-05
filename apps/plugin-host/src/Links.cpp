#include "Links.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/platform/Pipe.hpp"

#include <windows.h>

#include <chrono>
#include <future>
#include <optional>

namespace rmw::pluginhost
{

using namespace std::chrono_literals;
using json::Json;

// ---- inbox --------------------------------------------------------------------------------------

Inbox::Inbox()
    : m_event(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
}

Inbox::~Inbox()
{
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
    }
}

void Inbox::post(std::function<void()> task)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    SetEvent(m_event);
}

std::deque<std::function<void()>> Inbox::take()
{
    const std::scoped_lock lock(m_mutex);
    return std::exchange(m_tasks, {});
}

// ---- control ------------------------------------------------------------------------------------

ControlServer::ControlServer(std::wstring pipe, std::filesystem::path coreImage, std::shared_ptr<Inbox> inbox,
                             Handler handle)
{
    m_thread =
        std::jthread([pipe = std::move(pipe), coreImage = std::move(coreImage), inbox = std::move(inbox),
                      handle = std::move(handle)](const std::stop_token& stop) {
            auto server = platform::PipeServer::create(pipe);
            if (!server)
            {
                log::error("plugin host: cannot serve the control pipe: {}", server.error().toString());
                return;
            }
            while (!stop.stop_requested())
            {
                auto connection = server->accept(250ms);
                if (!connection)
                {
                    continue;
                }
                const auto image = platform::processImagePath(connection->peerProcessId());
                if (!image || !platform::samePath(*image, coreImage))
                {
                    log::warn("plugin host: refused a control client that is not rmw-core");
                    continue;
                }
                auto text = connection->receive(2s);
                if (!text)
                {
                    continue;
                }
                auto request = ipc::parseRequest(*text);
                if (!request)
                {
                    [[maybe_unused]] auto sent =
                        connection->send(ipc::errorText(nullptr, ipc::rpcError(ipc::RpcCode::InvalidRequest,
                                                                               request.error().message())),
                                         2s);
                    continue;
                }
                auto promise = std::make_shared<std::promise<std::string>>();
                auto answer = promise->get_future();
                inbox->post([promise, request = std::move(*request), handle] {
                    auto result = handle(request);
                    promise->set_value(result ? ipc::resultText(request.id, *result)
                                              : ipc::errorText(request.id, ipc::toRpcError(result.error())));
                });
                if (answer.wait_for(10s) != std::future_status::ready)
                {
                    [[maybe_unused]] auto sent = connection->send(
                        ipc::errorText(nullptr, ipc::rpcError(ipc::RpcCode::Busy, "the plugin host is busy")),
                        2s);
                    continue;
                }
                [[maybe_unused]] auto sent = connection->send(answer.get(), 2s);
            }
        });
}

ControlServer::~ControlServer() = default;

// ---- core link ----------------------------------------------------------------------------------

struct CoreLink::State
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<std::string, Json>> queue;
};

namespace
{

Result<Json> exchange(platform::PipeConnection& connection, std::int64_t id, std::string_view method,
                      const Json& params, std::chrono::milliseconds timeout)
{
    if (auto sent = connection.send(ipc::requestText(id, method, params), 2s); !sent)
    {
        return std::unexpected(std::move(sent.error()));
    }
    auto text = connection.receive(timeout);
    if (!text)
    {
        return std::unexpected(std::move(text.error()));
    }
    auto response = ipc::parseResponse(*text);
    if (!response)
    {
        return std::unexpected(std::move(response.error()));
    }
    if (response->error)
    {
        return std::unexpected(ipc::fromRpcError(*response->error));
    }
    return response->result;
}

} // namespace

CoreLink::CoreLink(std::filesystem::path coreImage)
    : m_state(std::make_shared<State>())
{
    m_thread = std::jthread([state = m_state, coreImage = std::move(coreImage)](const std::stop_token& stop) {
        std::optional<platform::PipeConnection> connection;
        std::int64_t nextId = 1;
        while (!stop.stop_requested())
        {
            std::pair<std::string, Json> call;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait_for(lock, 500ms,
                                     [&] { return !state->queue.empty() || stop.stop_requested(); });
                if (state->queue.empty())
                {
                    continue;
                }
                call = std::move(state->queue.front());
                state->queue.pop_front();
            }
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (!connection)
                {
                    auto name = platform::corePipeName();
                    auto opened = name ? platform::connectPipe(*name, 2s, coreImage)
                                       : Result<platform::PipeConnection>(std::unexpected(name.error()));
                    if (!opened)
                    {
                        log::warn("plugin host: the core is not reachable: {}", opened.error().toString());
                        break;
                    }
                    if (auto hello = exchange(*opened, nextId++, "hello", ipc::helloParams(kVersion), 5s);
                        !hello)
                    {
                        log::warn("plugin host: the core refused us: {}", hello.error().toString());
                        break;
                    }
                    connection.emplace(std::move(*opened));
                }
                auto result = exchange(*connection, nextId++, call.first, call.second, 120s);
                if (result)
                {
                    break;
                }
                if (result.error().code() == ErrorCode::NotFound ||
                    result.error().code() == ErrorCode::Timeout)
                {
                    connection.reset(); // the core restarted: once more on a new connection
                    continue;
                }
                log::warn("plugin host: {}: {}", call.first, result.error().message());
                break;
            }
        }
    });
}

CoreLink::~CoreLink()
{
    m_thread.request_stop();
    m_state->wake.notify_all();
}

void CoreLink::call(std::string method, Json params)
{
    {
        const std::scoped_lock lock(m_state->mutex);
        if (m_state->queue.size() >= 64)
        {
            log::warn("plugin host: too many calls to the core waiting; {} dropped", method);
            return;
        }
        m_state->queue.emplace_back(std::move(method), std::move(params));
    }
    m_state->wake.notify_all();
}

} // namespace rmw::pluginhost
