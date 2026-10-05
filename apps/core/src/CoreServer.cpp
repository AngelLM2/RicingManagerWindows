#include "CoreServer.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/platform/Customizers.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <array>
#include <format>

namespace rmw::core
{
namespace
{

using ipc::Json;
using namespace std::chrono_literals;

constexpr auto kHelloTimeout = 5s;
constexpr auto kPoll = 1s;
constexpr auto kSendTimeout = 10s;
constexpr std::array<std::string_view, 11> kMethods = {
    "status.get",    "rice.validate", "rice.apply",  "restore.original", "snapshot.list", "snapshot.restore",
    "monitors.list", "action.run",    "action.list", "module.inspect",   "rice.export",
};
constexpr std::array<std::string_view, 3> kActions = {"rice.apply", "restore.original", "appearance.toggle"};

std::chrono::steady_clock::rep nowTicks() noexcept
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

std::string utf8Path(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

Json diagnosticsJson(const domain::Diagnostics& diagnostics)
{
    Json items = Json::array();
    for (const auto& d : diagnostics.all())
    {
        items.push_back({{"severity", d.severity == domain::Severity::Error ? "error" : "warning"},
                         {"key", d.key},
                         {"line", d.line},
                         {"column", d.column},
                         {"message", d.message}});
    }
    return items;
}

Json reportJson(const engine::OperationReport& report)
{
    Json skipped = Json::array();
    for (const auto& s : report.skipped)
    {
        skipped.push_back({{"module", s.module}, {"field", s.field}, {"reason", s.reason}});
    }
    return Json{{"historyId", report.historyId},
                {"modules", report.modules},
                {"skipped", std::move(skipped)},
                {"warnings", report.warnings}};
}

struct LoadedRice
{
    std::filesystem::path folder;
    domain::Diagnostics diagnostics;
    Result<domain::Rice> rice = fail(ErrorCode::Unknown, "not loaded");
};

// Reads <folder>\rice.toml (untrusted) with the manifest size limit.
Result<LoadedRice> loadRice(const Json& params)
{
    const std::string path = json::stringOr(params, "path", "");
    if (path.empty())
    {
        return fail(ErrorCode::InvalidArgument, "missing 'path' (the rice folder)");
    }
    auto wide = utf8To16(path);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    LoadedRice loaded;
    loaded.folder = std::filesystem::path(*wide);
    std::error_code ec;
    if (!std::filesystem::is_directory(loaded.folder, ec))
    {
        return fail(ErrorCode::NotFound, std::format("'{}' is not a folder", path));
    }
    auto bytes = platform::readFileLimited(loaded.folder / L"rice.toml", limits::kManifestMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()).withContext("read rice.toml"));
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    loaded.rice = domain::parseRice(text, loaded.diagnostics);
    return loaded;
}

} // namespace

CoreServer::CoreServer(CoreOptions options)
    : m_options(std::move(options))
    , m_lastActivity(nowTicks())
{
}

CoreServer::~CoreServer()
{
    m_stop = true;
    m_workers.clear(); // joins
    if (m_helper)
    {
        m_helper->stop();
    }
}

Result<void> CoreServer::initialize()
{
    std::error_code ec;
    std::filesystem::create_directories(m_options.dataDirectory, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot create the data folder: {}", ec.message()),
                    ec.value());
    }
    auto state = engine::StateDb::open(m_options.dataDirectory / L"state.db");
    if (!state)
    {
        return std::unexpected(std::move(state.error()).withContext("open state.db"));
    }
    m_state = std::make_unique<engine::StateDb>(std::move(*state));
    m_store = std::make_unique<engine::ContentStore>(m_options.dataDirectory / L"store");

    auto logic = makeModuleLogic();
    if (!logic)
    {
        return std::unexpected(std::move(logic.error()));
    }
    m_logic = std::move(*logic);

    engine::IModuleRunner* runner = nullptr;
    if (m_options.inProcess)
    {
        m_systems = std::make_unique<ModuleSystems>();
        runner = &m_systems->runner;
    }
    else
    {
        auto self = platform::currentExecutable();
        if (!self)
        {
            return std::unexpected(std::move(self.error()));
        }
        m_helper = std::make_unique<HelperModuleRunner>(*self);
        runner = m_helper.get();
    }
    m_engine = std::make_unique<engine::RiceEngine>(*m_state, *m_store, m_logic->catalog, *runner);
    if (!m_options.testPauseAfterModule.empty())
    {
        log::warn("TEST HOOK: pausing {} ms after module {}", m_options.testPause.count(),
                  m_options.testPauseAfterModule);
        m_engine->setAfterModuleHook([this](std::string_view module) {
            if (module == m_options.testPauseAfterModule)
            {
                std::this_thread::sleep_for(m_options.testPause);
            }
        });
    }

    // A crash in the middle of an operation is always undone first (docs/03 §5).
    if (auto recovered = m_engine->recover(); !recovered)
    {
        log::error("could not undo the interrupted operation: {}", recovered.error().toString());
    }
    else if (*recovered)
    {
        log::warn("an interrupted operation was undone");
    }
    return {};
}

Result<bool> CoreServer::recoverOnly()
{
    return m_engine->recover();
}

int CoreServer::serve(platform::PipeServer& server)
{
    log::info("core serving {}", utf16To8Lossy(server.name()));
    while (!m_stop)
    {
        auto connection = server.accept(kPoll);
        // Finished client threads are joined as we go.
        std::erase_if(m_workers, [](const Worker& worker) { return worker.done->load(); });
        if (connection)
        {
            ++m_clients;
            m_lastActivity = nowTicks();
            auto done = std::make_shared<std::atomic<bool>>(false);
            m_workers.push_back({done, std::jthread([this, done, c = std::move(*connection)]() mutable {
                                     handleClient(std::move(c));
                                     *done = true;
                                 })});
            continue;
        }
        if (connection.error().code() != ErrorCode::Timeout)
        {
            log::warn("accept failed: {}", connection.error().toString());
        }
        const auto idleFor = std::chrono::steady_clock::duration(nowTicks() - m_lastActivity.load());
        if (m_clients == 0 && idleFor > m_options.idleExit)
        {
            log::info("idle for {} s: exiting", m_options.idleExit.count());
            break;
        }
    }
    m_stop = true;
    m_workers.clear(); // joins
    if (m_helper)
    {
        m_helper->stop();
    }
    return 0;
}

void CoreServer::handleClient(platform::PipeConnection connection)
{
    const auto done = [this] {
        m_lastActivity = nowTicks();
        --m_clients;
    };

    // The first message must be a compatible hello.
    auto first = connection.receive(kHelloTimeout);
    auto hello = first ? ipc::parseRequest(*first) : Result<ipc::Request>(std::unexpected(first.error()));
    if (!hello || hello->method != "hello")
    {
        done();
        return;
    }
    if (auto compatible = ipc::checkHello(hello->params); !compatible)
    {
        [[maybe_unused]] const auto refused =
            connection.send(ipc::errorText(hello->id, ipc::rpcError(ipc::RpcCode::Incompatible,
                                                                    compatible.error().message())),
                            kSendTimeout);
        done();
        return;
    }
    Json helloReply = ipc::helloParams(kVersion);
    helloReply["pid"] = GetCurrentProcessId();
    if (!connection.send(ipc::resultText(hello->id, helloReply), kSendTimeout))
    {
        done();
        return;
    }

    while (!m_stop)
    {
        auto message = connection.receive(kPoll);
        if (!message)
        {
            if (message.error().code() == ErrorCode::Timeout)
            {
                continue;
            }
            break; // disconnected or broken framing
        }
        m_lastActivity = nowTicks();
        auto request = ipc::parseRequest(*message);
        if (!request)
        {
            [[maybe_unused]] const auto refused =
                connection.send(ipc::errorText(nullptr, ipc::rpcError(ipc::RpcCode::InvalidRequest,
                                                                      request.error().message())),
                                kSendTimeout);
            break;
        }
        std::string reply;
        if (request->method == "shutdown")
        {
            m_stop = true;
            reply = ipc::resultText(request->id, Json::object());
        }
        else if (std::ranges::find(kMethods, request->method) == kMethods.end())
        {
            reply = ipc::errorText(request->id, ipc::rpcError(ipc::RpcCode::MethodNotFound, request->method));
        }
        else
        {
            auto result = dispatch(*request);
            if (result)
            {
                reply = ipc::resultText(request->id, *result);
            }
            else if (result.error().code() == ErrorCode::Busy)
            {
                reply =
                    ipc::errorText(request->id, ipc::rpcError(ipc::RpcCode::Busy, result.error().message()));
            }
            else
            {
                log::warn("{} failed: {}", request->method, result.error().toString());
                reply = ipc::errorText(request->id, ipc::toRpcError(result.error()));
            }
        }
        if (!request->id.is_null() && !connection.send(reply, kSendTimeout))
        {
            break;
        }
    }
    done();
}

Result<Json> CoreServer::exclusive(const std::function<Result<Json>()>& operation)
{
    const std::unique_lock lock(m_operation, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return fail(ErrorCode::Busy, "another operation is running; try again when it finishes");
    }
    if (m_options.testOperationDelay.count() > 0)
    {
        std::this_thread::sleep_for(m_options.testOperationDelay);
    }
    return operation();
}

Result<Json> CoreServer::dispatch(const ipc::Request& request)
{
    const std::string& method = request.method;
    if (method == "status.get")
    {
        return status();
    }
    if (method == "rice.validate")
    {
        return validateRice(request.params);
    }
    if (method == "rice.apply")
    {
        return exclusive([&] { return applyRice(request.params); });
    }
    if (method == "restore.original")
    {
        return exclusive([&] { return restore(request.params, true); });
    }
    if (method == "snapshot.restore")
    {
        return exclusive([&] { return restore(request.params, false); });
    }
    if (method == "snapshot.list")
    {
        auto history = m_state->history();
        if (!history)
        {
            return std::unexpected(std::move(history.error()));
        }
        Json entries = Json::array();
        for (const auto& entry : *history)
        {
            entries.push_back({{"id", entry.id}, {"label", entry.label}, {"appliedAt", entry.appliedAt}});
        }
        return entries;
    }
    if (method == "monitors.list")
    {
        return monitors();
    }
    if (method == "action.list")
    {
        return Json(kActions);
    }
    if (method == "module.inspect")
    {
        return exclusive([&] { return inspect(request.params); });
    }
    if (method == "rice.export")
    {
        return exclusive([&] { return exportRice(request.params); });
    }
    return runAction(request.params);
}

Result<Json> CoreServer::status()
{
    auto base = m_state->baseSnapshot();
    auto applied = m_state->appliedModules();
    auto history = m_state->history();
    auto needsRestore = m_engine->needsRestore();
    if (!base || !applied || !history || !needsRestore)
    {
        return fail(ErrorCode::Database, "cannot read state.db");
    }
    Json lastOperations = Json::array();
    for (std::size_t i = 0; i < history->size() && i < 5; ++i)
    {
        lastOperations.push_back({{"id", (*history)[i].id},
                                  {"label", (*history)[i].label},
                                  {"appliedAt", (*history)[i].appliedAt}});
    }
    const std::unique_lock probe(m_operation, std::try_to_lock);
    return Json{{"version", kVersion},
                {"dataDirectory", utf8Path(m_options.dataDirectory)},
                {"originalCapturedAt", *base ? Json((*base)->createdAt) : Json(nullptr)},
                {"managedModules", *applied},
                {"needsRestore", *needsRestore},
                {"busy", !probe.owns_lock()},
                {"otherTools", platform::detectOtherCustomizers()},
                {"history", std::move(lastOperations)}};
}

Result<Json> CoreServer::validateRice(const Json& params)
{
    auto loaded = loadRice(params);
    if (!loaded)
    {
        return std::unexpected(std::move(loaded.error()));
    }
    return Json{{"ok", loaded->rice.has_value()},
                {"name", loaded->rice ? loaded->rice->meta.name : std::string()},
                {"diagnostics", diagnosticsJson(loaded->diagnostics)}};
}

Result<Json> CoreServer::applyRice(const Json& params)
{
    auto loaded = loadRice(params);
    if (!loaded)
    {
        return std::unexpected(std::move(loaded.error()));
    }
    if (!loaded->rice)
    {
        return std::unexpected(std::move(loaded->rice.error()));
    }
    // The first apply captures "my original Windows": say if it was already customized.
    const auto base = m_state->baseSnapshot();
    const bool firstApply = base && !base->has_value();
    const std::vector<std::string> others =
        firstApply ? platform::detectOtherCustomizers() : std::vector<std::string>{};

    auto report = m_engine->apply(*loaded->rice, loaded->folder);
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    if (!others.empty())
    {
        std::string names;
        for (const auto& other : others)
        {
            names += (names.empty() ? "" : ", ") + other;
        }
        report->warnings.push_back(std::format("the original Windows was captured while {} {} active: it may "
                                               "already be customized, and {} may change the same settings",
                                               names, others.size() == 1 ? "was" : "were",
                                               others.size() == 1 ? "it" : "they"));
    }
    Json result = reportJson(*report);
    result["name"] = loaded->rice->meta.name;
    return result;
}

Result<Json> CoreServer::restore(const Json& params, bool original)
{
    const engine::RestoreOptions options{.force = json::boolOr(params, "force", false)};
    auto report = original ? m_engine->restoreOriginal(options)
                           : m_engine->restoreHistory(json::intOr(params, "id", 0), options);
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    return reportJson(*report);
}

Result<Json> CoreServer::monitors()
{
    auto list = m_logic->topology.monitors();
    if (!list)
    {
        return std::unexpected(std::move(list.error()));
    }
    Json monitors = Json::array();
    for (const auto& m : *list)
    {
        monitors.push_back({{"key", m.identityKey},
                            {"name", m.displayName},
                            {"primary", m.primary},
                            {"left", m.left},
                            {"top", m.top},
                            {"width", m.width()},
                            {"height", m.height()}});
    }
    return monitors;
}

engine::IModuleRunner& CoreServer::runner() noexcept
{
    if (m_systems)
    {
        return m_systems->runner;
    }
    return *m_helper;
}

Result<Json> CoreServer::inspect(const Json& params)
{
    const std::string module = json::stringOr(params, "module", "");
    if (m_logic->catalog.find(module) == nullptr)
    {
        return fail(ErrorCode::NotFound, std::format("unknown module '{}'", module));
    }
    auto captured = runner().capture(module);
    if (!captured)
    {
        return std::unexpected(std::move(captured.error()));
    }
    return Json{{"state", captured->state},
                {"fidelity", captured->fidelity == engine::Fidelity::Exact ? "exact" : "lossy"},
                {"reason", captured->lossyReason}};
}

Result<Json> CoreServer::exportRice(const Json& params)
{
    const std::string target = json::stringOr(params, "path", "");
    const std::string name = json::stringOr(params, "name", "My Windows");
    auto wideTarget = utf8To16(target);
    if (target.empty() || !wideTarget)
    {
        return fail(ErrorCode::InvalidArgument, "missing 'path' (the folder to create)");
    }
    const std::filesystem::path folder(*wideTarget);
    std::error_code ec;
    if (std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec))
    {
        return fail(ErrorCode::AlreadyExists, std::format("'{}' exists and is not empty", target));
    }

    // What Windows has now, module by module.
    modules::CapturedLook look;
    for (auto [module, slot] :
         {std::pair{"appearance", &look.appearance}, std::pair{"taskbar", &look.taskbar},
          std::pair{"cursors", &look.cursors}, std::pair{"wallpaper", &look.wallpaper}})
    {
        auto captured = runner().capture(module);
        if (!captured)
        {
            return std::unexpected(
                std::move(captured.error()).withContext(std::format("capture {}", module)));
        }
        *slot = std::move(captured->state);
    }
    auto monitors = m_logic->topology.monitors();
    if (!monitors)
    {
        return std::unexpected(std::move(monitors.error()));
    }
    look.monitors = std::move(*monitors);

    const auto extensionOf = [](std::string_view source) -> std::string {
        auto path = platform::expandEnvironment(source);
        if (!path)
        {
            return {};
        }
        // By content: TranscodedWallpaper has no extension, and a name can lie.
        return platform::sniffFileType(*path);
    };
    auto exported = modules::exportRice(name, look, extensionOf);
    if (!exported)
    {
        return std::unexpected(std::move(exported.error()));
    }

    Json notes = Json::array();
    for (const auto& note : exported->notes)
    {
        notes.push_back(note);
    }
    std::size_t copied = 0;
    for (const auto& file : exported->files)
    {
        auto source = platform::expandEnvironment(file.source);
        auto relative = utf8To16(file.relative);
        if (!source || !relative)
        {
            notes.push_back(std::format("{}: unreadable path", file.relative));
            continue;
        }
        const std::filesystem::path destination = folder / std::filesystem::path(*relative);
        std::filesystem::create_directories(destination.parent_path(), ec);
        const auto dot = file.relative.rfind('.');
        const std::string_view extension =
            dot == std::string::npos ? std::string_view{} : std::string_view(file.relative).substr(dot + 1);
        auto copy = platform::copyHashed(*source, destination, engine::assetSizeLimit(extension));
        if (!copy)
        {
            notes.push_back(std::format("{}: {}", file.relative, copy.error().message()));
            continue;
        }
        ++copied;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(exported->toml.data());
    if (auto written =
            platform::writeFileAtomic(folder / L"rice.toml", std::span(bytes, exported->toml.size()));
        !written)
    {
        return std::unexpected(std::move(written.error()));
    }

    // Read it back as any rice would be: what we wrote must be valid.
    domain::Diagnostics diagnostics;
    const auto check = domain::parseRice(exported->toml, diagnostics);
    return Json{{"path", target},
                {"files", copied},
                {"valid", check.has_value()},
                {"diagnostics", diagnosticsJson(diagnostics)},
                {"notes", std::move(notes)}};
}

Result<Json> CoreServer::runAction(const Json& params)
{
    const std::string name = json::stringOr(params, "name", "");
    const Json actionParams = json::memberOr(params, "params", Json::object());
    if (name == "rice.apply")
    {
        return exclusive([&] { return applyRice(actionParams); });
    }
    if (name == "restore.original")
    {
        return exclusive([&] { return restore(actionParams, true); });
    }
    if (name == "appearance.toggle")
    {
        return exclusive([&]() -> Result<Json> {
            // Flip whatever Windows shows now; the engine records it like any apply.
            auto now = runner().capture("appearance");
            if (!now)
            {
                return std::unexpected(std::move(now.error()));
            }
            const bool light = json::boolOr(now->state, "systemLight", true);
            domain::Rice rice;
            rice.meta.name = light ? "Dark mode" : "Light mode";
            rice.appearance = domain::AppearanceConfig{};
            rice.appearance->mode = light ? domain::ThemeMode::Dark : domain::ThemeMode::Light;
            auto report = m_engine->apply(rice, {});
            if (!report)
            {
                return std::unexpected(std::move(report.error()));
            }
            return reportJson(*report);
        });
    }
    return fail(ErrorCode::NotFound, std::format("unknown action '{}'", name));
}

} // namespace rmw::core
