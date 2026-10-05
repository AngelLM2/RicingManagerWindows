#include "CoreServer.hpp"

#include "JsonReports.hpp"
#include "Resident.hpp"
#include "WorkQueue.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/platform/Autostart.hpp"
#include "rmw/platform/Customizers.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <format>
#include <future>

namespace rmw::core
{
namespace
{

using ipc::Json;
using namespace std::chrono_literals;

constexpr auto kHelloTimeout = 5s;
constexpr auto kPoll = 1s;
constexpr auto kSendTimeout = 10s;
// Windows keeps broadcasting our own change for a moment after an operation (docs/03 §5.2).
constexpr auto kOwnChangesGrace = 4s;
constexpr auto kWindowAnswer = 10s;
constexpr std::array<std::string_view, 14> kMethods = {
    "status.get",       "rice.validate",   "rice.apply",    "restore.original", "snapshot.list",
    "snapshot.restore", "monitors.list",   "action.run",    "action.list",      "module.inspect",
    "rice.export",      "settings.reload", "autostart.get", "autostart.set",
};
constexpr std::string_view kScheduleMeta = "appearance_schedule";
// Outside state.db on purpose: a damaged database must not hide a crash loop.
constexpr std::wstring_view kStartupAttemptsFile = L"startup-attempts";

std::chrono::steady_clock::rep nowTicks() noexcept
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

int readStartupAttempts(const std::filesystem::path& file)
{
    auto bytes = platform::readFileLimited(file, 32);
    if (!bytes)
    {
        return 0;
    }
    const auto* text = reinterpret_cast<const char*>(bytes->data());
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text, text + bytes->size(), value);
    return ec == std::errc{} && value >= 0 && value < 1000 ? value : 0;
}

void writeStartupAttempts(const std::filesystem::path& file, int attempts)
{
    const std::string text = std::to_string(attempts);
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    if (auto written = platform::writeFileAtomic(file, std::span(bytes, text.size())); !written)
    {
        log::warn("cannot record the start: {}", written.error().toString());
    }
}

std::string timeText(domain::TimeOfDay time)
{
    return std::format("{:02}:{:02}", time.hour, time.minute);
}

// meta "appearance_schedule" = "HH:MM-HH:MM" (light at, dark at).
std::string scheduleText(const domain::AppearanceSchedule& schedule)
{
    return timeText(schedule.lightAt) + "-" + timeText(schedule.darkAt);
}

std::optional<domain::AppearanceSchedule> parseScheduleText(std::string_view text)
{
    const auto dash = text.find('-');
    if (dash == std::string_view::npos)
    {
        return std::nullopt;
    }
    auto light = domain::TimeOfDay::parse(text.substr(0, dash));
    auto dark = domain::TimeOfDay::parse(text.substr(dash + 1));
    if (!light || !dark)
    {
        return std::nullopt;
    }
    return domain::AppearanceSchedule{*light, *dark};
}

bool sameSchedule(const std::optional<domain::AppearanceSchedule>& a,
                  const std::optional<domain::AppearanceSchedule>& b) noexcept
{
    if (a.has_value() != b.has_value())
    {
        return false;
    }
    return !a || (a->lightAt == b->lightAt && a->darkAt == b->darkAt);
}

std::string utf8Path(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
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

// Marks an engine operation: change events until a moment after it are our own.
struct CoreServer::OperationMark
{
    explicit OperationMark(CoreServer& core) noexcept
        : m_core(&core)
    {
        m_core->m_operationRunning = true;
    }
    ~OperationMark()
    {
        m_core->m_lastOperationEnd = nowTicks();
        m_core->m_lastActivity = nowTicks();
        m_core->m_operationRunning = false;
    }
    OperationMark(const OperationMark&) = delete;
    OperationMark& operator=(const OperationMark&) = delete;
    OperationMark(OperationMark&&) = delete;
    OperationMark& operator=(OperationMark&&) = delete;

private:
    CoreServer* m_core;
};

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

    // docs/06 §6: count this start before anything that could crash; a healthy minute or a clean
    // exit sets it back to zero.
    const auto attemptsFile = m_options.dataDirectory / kStartupAttemptsFile;
    const int attempts = readStartupAttempts(attemptsFile);
    if (m_options.safeModeRequested)
    {
        m_safeMode = SafeModeReason::Requested;
    }
    else if (attempts >= kSafeModeAfterCrashes)
    {
        m_safeMode = SafeModeReason::Crashes;
    }
    writeStartupAttempts(attemptsFile, attempts + 1);
    if (m_safeMode != SafeModeReason::None)
    {
        log::warn("safe mode ({}): nothing runs automatically",
                  m_safeMode == SafeModeReason::Requested ? "requested" : "the last starts crashed");
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

    if (auto saved = m_state->meta(kScheduleMeta); saved && *saved)
    {
        m_schedule = parseScheduleText(**saved);
    }

    if (m_safeMode != SafeModeReason::None)
    {
        // The undo itself may be what crashes: leave it for an explicit restore.
        log::warn("safe mode: an interrupted operation, if any, is not undone automatically");
        return {};
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

bool CoreServer::wantsResidency() const
{
    if (m_safeMode != SafeModeReason::None || schedule())
    {
        return true;
    }
    return loadSettings(m_options.dataDirectory).settings.needsResidency();
}

void CoreServer::markHealthy() const noexcept
{
    std::error_code ec;
    std::filesystem::remove(m_options.dataDirectory / kStartupAttemptsFile, ec);
}

int CoreServer::run(platform::PipeServer& server)
{
    log::info("core serving {}", utf16To8Lossy(server.name()));
    auto window = platform::MessageWindow::create([this](const platform::SystemEvent& event) {
        if (m_resident != nullptr)
        {
            m_resident->onEvent(event);
        }
    });
    if (!window)
    {
        log::error("core window: {}", window.error().toString());
        return 1;
    }
    m_window = window->get();
    WorkQueue work;
    Resident resident(*this, *m_window, work);
    m_resident = &resident;
    resident.start();

    int code = 0;
    {
        const std::jthread accept([this, &server](const std::stop_token& stop) { acceptLoop(server, stop); });
        code = m_window->run();
        m_stop = true;
    } // joins the accept thread, which joins every client thread
    work.stop();
    m_resident = nullptr;
    m_window = nullptr;
    if (m_helper)
    {
        m_helper->stop();
    }
    markHealthy();
    log::info("core stopped");
    return code;
}

void CoreServer::acceptLoop(platform::PipeServer& server, const std::stop_token& stop)
{
    while (!m_stop && !stop.stop_requested())
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
    }
    m_stop = true;
    m_workers.clear(); // joins: client loops see m_stop within a poll
}

bool CoreServer::idleExpired() noexcept
{
    if (m_clients > 0 || m_operationRunning)
    {
        return false;
    }
    const auto idleFor = std::chrono::steady_clock::duration(nowTicks() - m_lastActivity.load());
    return idleFor > m_options.idleExit;
}

bool CoreServer::ownChangesSettling() const noexcept
{
    if (m_operationRunning)
    {
        return true;
    }
    const auto since = std::chrono::steady_clock::duration(nowTicks() - m_lastOperationEnd.load());
    return since < kOwnChangesGrace;
}

std::optional<domain::AppearanceSchedule> CoreServer::schedule() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_schedule;
}

void CoreServer::setSchedule(std::optional<domain::AppearanceSchedule> schedule)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (sameSchedule(m_schedule, schedule))
        {
            return;
        }
        m_schedule = schedule;
    }
    const std::string text = schedule ? scheduleText(*schedule) : std::string();
    if (auto saved =
            m_state->setMeta(kScheduleMeta, schedule ? std::optional<std::string_view>(text) : std::nullopt);
        !saved)
    {
        log::warn("cannot save the appearance schedule: {}", saved.error().toString());
    }
    log::info("appearance schedule: {}", schedule ? text : "off");
    notifyResident();
}

void CoreServer::notifyResident()
{
    if (m_window != nullptr && m_resident != nullptr)
    {
        Resident* resident = m_resident;
        m_window->post([resident] { resident->refresh(); });
    }
}

Result<Json> CoreServer::onWindow(const std::function<Json()>& task)
{
    auto promise = std::make_shared<std::promise<Json>>();
    auto answer = promise->get_future();
    m_window->post([promise, task] {
        try
        {
            promise->set_value(task());
        }
        catch (...)
        {
            promise->set_value(Json::object());
        }
    });
    if (answer.wait_for(kWindowAnswer) != std::future_status::ready)
    {
        return fail(ErrorCode::Timeout, "the core did not answer in time");
    }
    return answer.get();
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
    helloReply["dataDirectory"] = utf8Path(m_options.dataDirectory);
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
            m_window->quit(0);
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
    const OperationMark mark(*this);
    if (m_options.testOperationDelay.count() > 0)
    {
        std::this_thread::sleep_for(m_options.testOperationDelay);
    }
    return operation();
}

Result<Json> CoreServer::perform(const domain::ActionCall& action, Origin origin)
{
    Json params = Json::object();
    if (!action.ricePath.empty())
    {
        params["path"] = action.ricePath;
    }
    const std::scoped_lock lock(m_operation); // automatic work waits its turn
    const OperationMark mark(*this);
    return runAction(action.name, params, origin);
}

Result<std::vector<std::string>> CoreServer::detachDrifted(const std::string& module)
{
    const std::scoped_lock lock(m_operation);
    if (ownChangesSettling())
    {
        return std::vector<std::string>{}; // what changed was probably us
    }
    auto drifted = m_engine->driftedFields(module);
    if (!drifted || drifted->empty())
    {
        return drifted;
    }
    if (auto detached = m_engine->detach(module, *drifted); !detached)
    {
        return std::unexpected(std::move(detached.error()));
    }
    std::string names;
    for (const auto& field : *drifted)
    {
        names += (names.empty() ? "" : ", ") + field;
    }
    log::info("{}: changed outside ({}); no longer managed", module, names);
    return drifted;
}

Result<void> CoreServer::reapply(const std::vector<std::string>& modules)
{
    const std::scoped_lock lock(m_operation);
    const OperationMark mark(*this);
    auto report = m_engine->reapplyManaged(modules);
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    for (const auto& module : report->modules)
    {
        log::info("reapplied {}", module);
    }
    return {};
}

Result<bool> CoreServer::syncSchedule()
{
    const auto current = schedule();
    if (!current)
    {
        return false;
    }
    const std::scoped_lock lock(m_operation);
    const OperationMark mark(*this);
    domain::Rice rice;
    rice.meta.name = "Scheduled appearance";
    rice.appearance = domain::AppearanceConfig{};
    rice.appearance->mode = domain::ThemeMode::Schedule;
    rice.appearance->schedule = *current;
    auto report = m_engine->applyAutomatic(rice, {}, rice.meta.name);
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    if (!report->modules.empty())
    {
        log::info("scheduled appearance applied");
    }
    return !report->modules.empty();
}

Result<std::vector<std::string>> CoreServer::monitorKeys()
{
    const std::scoped_lock lock(m_operation);
    auto list = m_logic->topology.monitors();
    if (!list)
    {
        return std::unexpected(std::move(list.error()));
    }
    std::vector<std::string> keys;
    for (const auto& monitor : *list)
    {
        keys.push_back(monitor.identityKey);
    }
    return keys;
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
        return exclusive([&] { return applyRice(request.params, Origin::User); });
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
    if (method == "settings.reload")
    {
        Resident* resident = m_resident;
        return onWindow([resident] { return resident->reloadSettings(); });
    }
    if (method == "autostart.get" || method == "autostart.set")
    {
        return autostart(request.params, method == "autostart.set");
    }
    // action.run
    const std::string name = json::stringOr(request.params, "name", "");
    const Json actionParams = json::memberOr(request.params, "params", Json::object());
    return exclusive([&] { return runAction(name, actionParams, Origin::User); });
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
    Json safeMode = nullptr;
    if (m_safeMode != SafeModeReason::None)
    {
        safeMode = m_safeMode == SafeModeReason::Requested ? "requested" : "crashes";
    }
    Json schedule = nullptr;
    if (const auto current = this->schedule())
    {
        schedule = {{"lightAt", timeText(current->lightAt)}, {"darkAt", timeText(current->darkAt)}};
    }
    const std::unique_lock probe(m_operation, std::try_to_lock);
    return Json{{"version", kVersion},
                {"dataDirectory", utf8Path(m_options.dataDirectory)},
                {"originalCapturedAt", *base ? Json((*base)->createdAt) : Json(nullptr)},
                {"managedModules", *applied},
                {"needsRestore", *needsRestore},
                {"busy", !probe.owns_lock()},
                {"safeMode", std::move(safeMode)},
                {"schedule", std::move(schedule)},
                {"resident", m_resident != nullptr ? m_resident->status() : Json(nullptr)},
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

Result<Json> CoreServer::runAction(std::string_view name, const Json& params, Origin origin)
{
    if (name == "rice.apply")
    {
        return applyRice(params, origin);
    }
    if (name == "restore.original")
    {
        return restore(params, true);
    }
    if (name == "appearance.toggle" || name == "appearance.dark" || name == "appearance.light")
    {
        return setAppearance(name, origin);
    }
    return fail(ErrorCode::NotFound, std::format("unknown action '{}'", name));
}

Result<Json> CoreServer::applyRice(const Json& params, Origin origin)
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
    const domain::Rice& rice = *loaded->rice;
    // The first apply captures "my original Windows": say if it was already customized.
    const auto base = m_state->baseSnapshot();
    const bool firstApply = base && !base->has_value();
    const std::vector<std::string> others =
        firstApply ? platform::detectOtherCustomizers() : std::vector<std::string>{};

    auto report =
        origin == Origin::User
            ? m_engine->apply(rice, loaded->folder)
            : m_engine->applyAutomatic(rice, loaded->folder, std::format("Automatic: {}", rice.meta.name));
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    if (rice.appearance)
    {
        // A scheduled rice keeps the core around to switch light/dark on time.
        setSchedule(rice.appearance->mode == domain::ThemeMode::Schedule
                        ? std::optional(rice.appearance->schedule.value_or(domain::AppearanceSchedule{}))
                        : std::nullopt);
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
    result["name"] = rice.meta.name;
    return result;
}

Result<Json> CoreServer::setAppearance(std::string_view action, Origin origin)
{
    domain::ThemeMode mode = action == "appearance.dark" ? domain::ThemeMode::Dark : domain::ThemeMode::Light;
    if (action == "appearance.toggle")
    {
        // Flip whatever Windows shows now.
        auto now = runner().capture("appearance");
        if (!now)
        {
            return std::unexpected(std::move(now.error()));
        }
        mode = json::boolOr(now->state, "systemLight", true) ? domain::ThemeMode::Dark
                                                             : domain::ThemeMode::Light;
    }
    // A schedule stays: this choice lasts until its next switch.
    domain::Rice rice;
    rice.meta.name = mode == domain::ThemeMode::Dark ? "Dark mode" : "Light mode";
    rice.appearance = domain::AppearanceConfig{};
    rice.appearance->mode = mode;
    auto report = origin == Origin::User ? m_engine->apply(rice, {})
                                         : m_engine->applyAutomatic(rice, {}, rice.meta.name);
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    return reportJson(*report);
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
    setSchedule(std::nullopt); // going back ends any schedule
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
    auto applied = m_state->applied(module);
    if (!applied)
    {
        return std::unexpected(std::move(applied.error()));
    }
    Json managed = Json::array();
    for (const auto& [field, value] : applied->items())
    {
        managed.push_back(field);
    }
    return Json{{"state", captured->state},
                {"fidelity", captured->fidelity == engine::Fidelity::Exact ? "exact" : "lossy"},
                {"reason", captured->lossyReason},
                {"managed", std::move(managed)}};
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

Result<Json> CoreServer::autostart(const Json& params, bool change)
{
    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    if (change)
    {
        const bool enabled = json::boolOr(params, "enabled", false);
        if (auto set = platform::setAutostart(enabled, *self); !set)
        {
            return std::unexpected(std::move(set.error()));
        }
        log::info("start with Windows: {}", enabled ? "on" : "off");
    }
    auto state = platform::autostartState(*self);
    if (!state)
    {
        return std::unexpected(std::move(state.error()));
    }
    return Json{{"enabled", state->registered && state->current},
                {"registered", state->registered},
                {"current", state->current},
                {"disabledByUser", state->disabledByUser},
                {"command", utf16To8Lossy(state->command)}};
}

} // namespace rmw::core
