#include "ApplyHelper.hpp"

#include "Composition.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <format>

namespace rmw::core
{
namespace
{

using engine::Json;
using namespace std::chrono_literals;

constexpr auto kHelperIdle = 60s; // the helper quits when the core has not needed it for a while

} // namespace

Json captureToJson(const engine::CaptureResult& capture)
{
    return Json{{"state", capture.state},
                {"fidelity", capture.fidelity == engine::Fidelity::Exact ? "exact" : "lossy"},
                {"reason", capture.lossyReason}};
}

Result<engine::CaptureResult> captureFromJson(const Json& json)
{
    if (!json.is_object() || !json.contains("state") || !json["state"].is_object())
    {
        return fail(ErrorCode::InvalidData, "the apply helper sent a malformed capture");
    }
    engine::CaptureResult capture;
    capture.state = json["state"];
    capture.fidelity = json::stringOr(json, "fidelity", "exact") == "lossy" ? engine::Fidelity::Lossy
                                                                            : engine::Fidelity::Exact;
    capture.lossyReason = json::stringOr(json, "reason", "");
    return capture;
}

// ---- core side --------------------------------------------------------------------------------------

HelperModuleRunner::HelperModuleRunner(std::filesystem::path coreExecutable,
                                       std::chrono::milliseconds timeout)
    : m_executable(std::move(coreExecutable))
    , m_timeout(timeout)
{
}

void HelperModuleRunner::stop() noexcept
{
    m_connection.reset();
    if (m_child)
    {
        m_child->terminate();
        m_child.reset();
    }
}

Result<void> HelperModuleRunner::start()
{
    stop();
    auto suffix = platform::randomHex(16);
    if (!suffix)
    {
        return std::unexpected(std::move(suffix.error()));
    }
    const std::wstring name =
        LR"(\\.\pipe\RicingManagerWindows-helper-)" + std::wstring(suffix->begin(), suffix->end());
    auto server = platform::PipeServer::create(name);
    if (!server)
    {
        return std::unexpected(std::move(server.error()));
    }
    auto child =
        platform::ChildProcess::spawn(m_executable, L"--apply-helper --pipe " + name,
                                      {.memoryLimitBytes = kHelperMemoryLimit, .inheritHandles = {}});
    if (!child)
    {
        return std::unexpected(std::move(child.error()));
    }
    auto connection = server->accept(10s);
    if (!connection)
    {
        child->terminate();
        return std::unexpected(std::move(connection.error()).withContext("start the apply helper"));
    }
    // Only our own child may answer on this pipe.
    if (connection->peerProcessId() != child->pid())
    {
        child->terminate();
        return fail(ErrorCode::AccessDenied, "an unexpected process connected to the apply helper pipe");
    }
    m_child = std::make_unique<platform::ChildProcess>(std::move(*child));
    m_connection = std::make_unique<platform::PipeConnection>(std::move(*connection));

    auto hello = call("hello", ipc::helloParams(kVersion));
    if (!hello)
    {
        stop();
        return std::unexpected(std::move(hello.error()));
    }
    if (auto compatible = ipc::checkHello(*hello); !compatible)
    {
        stop();
        return compatible;
    }
    return {};
}

Result<Json> HelperModuleRunner::call(std::string_view method, const Json& params)
{
    if (!m_connection || !m_child || !m_child->running())
    {
        if (method == "hello")
        {
            return fail(ErrorCode::InvalidArgument, "no helper connection");
        }
        if (auto started = start(); !started)
        {
            return std::unexpected(std::move(started.error()));
        }
    }
    const std::int64_t id = m_nextId++;
    const auto failed = [&](Error error) -> Result<Json> {
        stop(); // a helper that missed a deadline or broke the protocol is not reused
        return std::unexpected(std::move(error));
    };
    if (auto sent = m_connection->send(ipc::requestText(id, method, params), m_timeout); !sent)
    {
        return failed(std::move(sent.error()).withContext("send to the apply helper"));
    }
    auto reply = m_connection->receive(m_timeout);
    if (!reply)
    {
        if (reply.error().code() == ErrorCode::Timeout)
        {
            return failed(
                Error(ErrorCode::Timeout,
                      std::format("the apply helper did not answer '{}' within {} s (Explorer may be hung); "
                                  "it was stopped",
                                  method, m_timeout.count() / 1000)));
        }
        return failed(std::move(reply.error()).withContext("the apply helper stopped"));
    }
    auto response = ipc::parseResponse(*reply);
    if (!response || response->id != id)
    {
        return failed(Error(ErrorCode::InvalidData, "the apply helper sent an unexpected reply"));
    }
    if (response->error)
    {
        return std::unexpected(ipc::fromRpcError(*response->error));
    }
    return std::move(response->result);
}

Result<engine::CaptureResult> HelperModuleRunner::capture(std::string_view moduleId)
{
    auto result = call("module.capture", Json{{"module", moduleId}});
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    return captureFromJson(*result);
}

Result<void> HelperModuleRunner::apply(std::string_view moduleId, const Json& desired)
{
    auto result = call("module.apply", Json{{"module", moduleId}, {"desired", desired}});
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}

// ---- helper side --------------------------------------------------------------------------------------

int runApplyHelper(const std::wstring& pipeName)
{
    auto self = platform::currentExecutable();
    if (!self)
    {
        log::error("apply helper: {}", self.error().toString());
        return 1;
    }
    // The core serves this pipe from the same executable.
    auto connection = platform::connectPipe(pipeName, 10s, *self);
    if (!connection)
    {
        log::error("apply helper: cannot reach the core: {}", connection.error().toString());
        return 1;
    }
    ModuleSystems systems;

    while (true)
    {
        auto message = connection->receive(kHelperIdle);
        if (!message)
        {
            // Idle or the core went away: either way this helper is done.
            return message.error().code() == ErrorCode::Timeout ||
                           message.error().code() == ErrorCode::NotFound
                       ? 0
                       : 1;
        }
        auto request = ipc::parseRequest(*message);
        if (!request)
        {
            log::error("apply helper: bad request: {}", request.error().message());
            return 1; // the core never sends these: stop talking
        }

        Result<Json> result = Json::object();
        const std::string module = json::stringOr(request->params, "module", "");
        if (request->method == "hello")
        {
            result = ipc::checkHello(request->params).transform([] { return ipc::helloParams(kVersion); });
        }
        else if (request->method == "module.capture")
        {
            result = systems.runner.capture(module).transform(
                [](const engine::CaptureResult& c) { return captureToJson(c); });
        }
        else if (request->method == "module.apply")
        {
            const Json desired = json::memberOr(request->params, "desired", Json::object());
            result = systems.runner.apply(module, desired).transform([] { return Json::object(); });
        }
        else
        {
            if (auto sent = connection->send(
                    ipc::errorText(request->id, ipc::rpcError(ipc::RpcCode::MethodNotFound, request->method)),
                    kHelperTimeout);
                !sent)
            {
                return 1;
            }
            continue;
        }

        const std::string reply = result ? ipc::resultText(request->id, *result)
                                         : ipc::errorText(request->id, ipc::toRpcError(result.error()));
        if (!result)
        {
            log::warn("apply helper: {} {} failed: {}", request->method, module, result.error().toString());
        }
        if (auto sent = connection->send(reply, kHelperTimeout); !sent)
        {
            return 1;
        }
    }
}

} // namespace rmw::core
