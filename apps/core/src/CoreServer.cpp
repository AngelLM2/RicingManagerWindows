#include "CoreServer.hpp"

#include "Import.hpp"
#include "JsonReports.hpp"
#include "Resident.hpp"
#include "WorkQueue.hpp"

#include "rmw/bar/Plan.hpp"
#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/domain/CursorFiles.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/modules/Wallpaper.hpp"
#include "rmw/platform/Apps.hpp"
#include "rmw/platform/Autostart.hpp"
#include "rmw/platform/Customizers.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/NativeTaskbar.hpp"
#include "rmw/plugins/HostPlan.hpp"
#include "rmw/tiling/Plan.hpp"
#include "rmw/tiling/Shared.hpp"
#include "rmw/video/Plan.hpp"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

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
constexpr std::chrono::milliseconds kAcceptBackOff{50};
constexpr std::chrono::milliseconds kAcceptMaxBackOff{2000};
constexpr auto kSendTimeout = 10s;
// Windows keeps broadcasting our own change for a moment after an operation (docs/03 §5.2).
constexpr auto kOwnChangesGrace = 4s;
constexpr auto kWindowAnswer = 10s;
constexpr std::array<std::string_view, 26> kMethods = {
    "status.get",       "rice.validate",   "rice.apply",     "restore.original", "snapshot.list",
    "snapshot.restore", "monitors.list",   "action.run",     "action.list",      "module.inspect",
    "rice.export",      "settings.reload", "autostart.get",  "autostart.set",    "plugin.list",
    "plugin.approve",   "plugin.enable",   "plugin.disable", "plugin.dev",       "plugin.install",
    "plugin.remove",    "plugin.status",   "plugin.report",  "apps.list",        "rice.import",
    "recover",
};
constexpr std::string_view kScheduleMeta = "appearance_schedule";
constexpr std::string_view kRotationMeta = "wallpaper_rotation";
// The slideshow plan of an apply that is running: it keeps its pages from the apply's clean-up
// and becomes the plan only once the apply worked (a crash in between leaves no plan behind).
constexpr std::string_view kRotationPendingMeta = "wallpaper_rotation_pending";
constexpr std::string_view kBarPlanMeta = "bar_plan";
constexpr std::string_view kWindowLookMeta = "window_look"; // the applied rice's [windows] (M14)
constexpr std::string_view kMachineIconsMeta = "machine_icons"; // rmw-elevated changed Shell Icons (M14)
constexpr std::string_view kBarOffMeta = "bar_off";
constexpr std::string_view kBarSourceMeta = "bar_source";
constexpr std::string_view kBarWorker = "bar";
constexpr std::string_view kVideoPlanMeta = "video_plan"; // also keeps the videos from the store's GC
constexpr std::string_view kVideoWorker = "wallpaper";
// Decoding 1080p takes ~55-110 MB (ADR-0006); 4K and two monitors more. A leak ends at this.
constexpr std::uint64_t kVideoMemoryLimit = std::uint64_t{768} << 20;
constexpr std::uint64_t kVideoMaxBytes = std::uint64_t{4} << 30;        // what one video may weigh
constexpr std::uint64_t kVideoStatusMaxBytes = std::uint64_t{64} << 10; // rmw-wallpaper's status.json
constexpr std::uint64_t kBarMemoryLimit = std::uint64_t{96} << 20;      // the bar's budget is 25 MB
constexpr std::string_view kTilerWorker = "tiler";
constexpr std::uint64_t kTilerMemoryLimit = std::uint64_t{64} << 20; // it keeps a list of windows
constexpr auto kTilerLeaveWait = 3s; // putting a few hundred windows back takes well under a second
constexpr std::string_view kPluginWorker = "plugins";
// 16 MB per plugin VM (docs/12 §6), a dozen plugins and the host itself.
constexpr std::uint64_t kPluginHostMemoryLimit = std::uint64_t{256} << 20;
// A plugin that brings the whole host down (hung in a C function, ate its memory) is stopped
// sooner than one whose callbacks just fail (3 times, in the host): twice in five minutes.
constexpr int kBlamesToDisable = 2;
constexpr auto kBlameWindow = std::chrono::minutes(5);
constexpr std::string_view kRiceCurrentMeta = "rice_current";   // the folder of the rice applied last
constexpr std::string_view kRicePreviousMeta = "rice_previous"; // the one before it (rice.undo)
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

// Windows' accent color (DWM keeps it as ABGR), for particles in "palette.accent".
domain::Color systemAccent()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\DWM)", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return {0x00, 0x78, 0xD4};
    }
    return {static_cast<std::uint8_t>(value & 0xFFU), static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
            static_cast<std::uint8_t>((value >> 16U) & 0xFFU)};
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

// rmw-elevated's request for a rice's [icons.machine]: each .ico read from the rice's folder (the
// parser checked its path), checked, and named by its hash (rmw-elevated checks the bytes it reads
// against it).
Result<machine::Command> machineCommand(const domain::Rice& rice, const std::filesystem::path& folder,
                                        engine::ContentStore& store)
{
    machine::Command command{machine::Command::Kind::SetShellIcons, {}};
    if (!rice.icons)
    {
        return command;
    }
    engine::StoreAssetSink sink(store, folder);
    for (const auto& [icon, file] : rice.icons->machine)
    {
        auto bytes = sink.readPackageFile(file, limits::kIconMaxBytes);
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()));
        }
        if (auto valid = domain::validateIconFile(*bytes); !valid)
        {
            return std::unexpected(std::move(valid.error()).withContext(file));
        }
        auto digest = platform::sha256(*bytes);
        auto relative = utf8To16(file);
        if (!digest || !relative)
        {
            return fail(ErrorCode::InvalidData, std::format("cannot read {}", file));
        }
        std::error_code ec;
        const auto source = std::filesystem::weakly_canonical(folder / *relative, ec);
        command.icons.push_back({icon, platform::toHex(*digest), source.native()});
    }
    return command;
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
    if (m_blame != nullptr)
    {
        CloseHandle(m_blame);
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

    // A damaged state.db must not keep the core from ever starting (safe mode needs it too): it is
    // moved aside and a new one begins, the original snapshot coming back from its copy below.
    auto state = engine::StateDb::openOrReplace(m_options.dataDirectory / L"state.db", {}, m_stateMovedTo);
    if (!state)
    {
        return std::unexpected(std::move(state.error()).withContext("open state.db"));
    }
    if (m_stateMovedTo)
    {
        log::error("state.db was damaged: it was moved to {} and a new one is used; the history is gone, the "
                   "original Windows comes back from its copy",
                   utf16To8Lossy(m_stateMovedTo->native()));
    }
    m_state = std::make_unique<engine::StateDb>(std::move(*state));
    m_store = std::make_unique<engine::ContentStore>(m_options.dataDirectory / L"store");
    m_plugins = std::make_unique<PluginManager>(m_options.dataDirectory, *m_state);
    if (auto base = platform::corePipeName())
    {
        const std::string token = platform::randomHex(16).value_or(std::to_string(GetCurrentProcessId()));
        m_bridgePipe = *base + L"-bridge-" + utf8To16Lossy(token);
        m_controlPipe = *base + L"-plugins-" + utf8To16Lossy(token);
    }
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    m_blame = CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0,
                                 sizeof(plugins::BlameRecord), nullptr);

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
        m_helper = std::make_unique<HelperModuleRunner>(*self, m_options.dataDirectory);
        runner = m_helper.get();
    }
    m_engine = std::make_unique<engine::RiceEngine>(*m_state, *m_store, m_logic->catalog, *runner);
    m_engine->setOriginalBackup(m_options.dataDirectory / L"original.json");
    m_engine->setCoreState([this] { return coreState(); });
    if (auto original = m_engine->originalSnapshot(); !original)
    {
        log::error("original snapshot: {}", original.error().toString());
    }
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
    if (auto saved = m_state->meta(kBarPlanMeta); saved && *saved)
    {
        m_barPlan = **saved;
    }
    if (auto saved = m_state->meta(kWindowLookMeta); saved && *saved)
    {
        if (auto look = tiling::parseLook(**saved))
        {
            m_windowLook = std::move(*look);
        }
    }
    if (auto saved = m_state->meta(kVideoPlanMeta); saved && *saved)
    {
        m_videoPlan = **saved;
    }
    if (auto saved = m_state->meta(kBarOffMeta); saved && *saved)
    {
        m_barOff = **saved == "1";
    }
    if (auto saved = m_state->meta(kBarSourceMeta); saved && *saved)
    {
        if (auto folder = utf8To16(**saved))
        {
            m_barSource = std::filesystem::path(*folder);
        }
    }
    if (auto saved = m_state->meta(kRotationMeta); saved && *saved)
    {
        auto plan = json::parse(**saved);
        if (plan && plan->is_object())
        {
            m_rotation = std::move(*plan);
        }
    }
    // The slideshow plan of an apply a crash interrupted: that apply is undone below.
    if (auto pending = m_state->meta(kRotationPendingMeta); pending && *pending)
    {
        log::warn("the slideshow plan of an interrupted apply is dropped");
        [[maybe_unused]] auto dropped = m_state->setMeta(kRotationPendingMeta, std::nullopt);
    }

    if (m_safeMode != SafeModeReason::None)
    {
        // The undo itself may be what crashes: leave it for an explicit restore (or `rmw recover`,
        // which can also forget it without undoing it).
        log::warn("safe mode: an interrupted operation, if any, is not undone automatically");
        return {};
    }
    // A crash in the middle of an operation is always undone first (docs/03 §5).
    auto recovered = m_engine->recover();
    if (!recovered && recovered.error().code() == ErrorCode::Timeout)
    {
        // Windows may still be busy with the change the crash interrupted (a theme switch takes
        // its time): one more try once it has settled.
        log::warn("undo timed out ({}); trying again", recovered.error().message());
        std::this_thread::sleep_for(std::chrono::seconds(3));
        recovered = m_engine->recover();
    }
    if (!recovered)
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
    if (m_safeMode != SafeModeReason::None || schedule() || !rotation().empty() || barPlan() || videoPlan())
    {
        return true;
    }
    if (m_plugins && !m_plugins->hostPlan(std::nullopt).plugins.empty())
    {
        return true; // plugins switched on (actions, rules) run without a rice
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
    if (auto supervisor = platform::Supervisor::create(
            [this](const platform::WorkerEvent& event) { onWorkerEvent(event); }))
    {
        m_supervisor = std::move(*supervisor);
    }
    else
    {
        log::warn("no worker supervisor: {}", supervisor.error().toString());
    }
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
    // The tiler hides the windows of the other workspaces: asked, it puts every one back and leaves.
    // Ended with the other workers it could not, and they stayed hidden (a clean exit, the tray's
    // Exit, the uninstaller's `rmw shutdown --wait`).
    leaveTiler();
    m_supervisor.reset(); // ends the bar and the rest...
    putBackTiledWindows(); // (a tiler that did not leave in time left its list behind)
    if (barPlan())
    {
        // ...so Windows' taskbar comes back, and its tray gets the icons the bar's tray held.
        platform::showNativeTaskbars();
        platform::askAppsForTrayIcons();
    }
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
    auto backOff = kAcceptBackOff;
    while (!m_stop && !stop.stop_requested())
    {
        auto connection = server.accept(kPoll);
        // Finished client threads are joined as we go.
        std::erase_if(m_workers, [](const Worker& worker) { return worker.done->load(); });
        if (connection)
        {
            backOff = kAcceptBackOff;
            ++m_clients;
            m_lastActivity = nowTicks();
            auto done = std::make_shared<std::atomic<bool>>(false);
            try
            {
                m_workers.push_back({done, std::jthread([this, done, c = std::move(*connection)]() mutable {
                                         // Nothing a client sends may end the core (docs/08 §3 rule 5).
                                         try
                                         {
                                             handleClient(std::move(c));
                                         }
                                         catch (const std::exception& e)
                                         {
                                             log::error("client: {}", e.what());
                                         }
                                         catch (...)
                                         {
                                             log::error("client: unknown exception");
                                         }
                                         *done = true;
                                     })});
            }
            catch (const std::exception& e)
            {
                --m_clients; // no thread for it: the connection closed with the lambda
                log::error("client: {}", e.what());
            }
            continue;
        }
        if (connection.error().code() == ErrorCode::Timeout)
        {
            continue;
        }
        // The server recycles its instance; waiting a little keeps an error that comes back at once
        // (no handles left, a pipe in a bad state) from spinning a core.
        log::warn("accept failed: {}", connection.error().toString());
        std::this_thread::sleep_for(backOff);
        backOff = std::min<std::chrono::milliseconds>(backOff * 2, kAcceptMaxBackOff);
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

Json CoreServer::rotation() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_rotation;
}

void CoreServer::setRotation(const Json& plan)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_rotation == plan)
        {
            return;
        }
        m_rotation = plan;
    }
    // The pages live in the store: the plan in meta keeps them from garbage collection.
    if (auto saved = m_state->setMeta(
            kRotationMeta, plan.empty() ? std::nullopt : std::optional<std::string>(json::dump(plan)));
        !saved)
    {
        log::warn("cannot save the slideshow plan: {}", saved.error().toString());
    }
    log::info("slideshows turned by the core: {}", plan.size());
    notifyResident();
}

std::optional<std::string> CoreServer::barPlan() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_barPlan;
}

bool CoreServer::barOff() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_barOff;
}

void CoreServer::setStartOptions(bool winKey, std::string language)
{
    std::optional<std::string> plan;
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_startWinKey == winKey && m_startLanguage == language)
        {
            return;
        }
        m_startWinKey = winKey;
        m_startLanguage = std::move(language);
        plan = m_barPlan;
    }
    if (plan)
    {
        writeBarPlan(plan, false); // the bar hears of it; switched off, it stays off
    }
}

void CoreServer::setBarPlan(const std::optional<std::string>& plan)
{
    writeBarPlan(plan, true);
}

void CoreServer::writeBarPlan(const std::optional<std::string>& given, bool newBars)
{
    // The user's Win key choice (settings.toml) goes with the rice's bars.
    std::optional<std::string> plan = given;
    if (plan)
    {
        if (auto parsed = json::parse(*plan, {.maxBytes = limits::kManifestMaxBytes}); parsed && parsed->is_object())
        {
            const std::scoped_lock lock(m_scheduleMutex);
            (*parsed)["start"]["winKey"] = m_startWinKey;
            (*parsed)["start"]["language"] = m_startLanguage;
            plan = json::dumpPretty(*parsed);
        }
    }
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_barPlan == plan)
        {
            return;
        }
        m_barPlan = plan;
        if (plan && newBars)
        {
            m_barOff = false; // a new bar is meant to be seen
        }
    }
    if (auto saved =
            m_state->setMeta(kBarPlanMeta, plan ? std::optional<std::string_view>(*plan) : std::nullopt);
        !saved)
    {
        log::warn("cannot save the bar plan: {}", saved.error().toString());
    }
    if (plan && newBars)
    {
        [[maybe_unused]] const auto cleared = m_state->setMeta(kBarOffMeta, std::nullopt);
    }
    // rmw-bar follows the file: rewritten, it rebuilds its bars; gone, it leaves.
    const auto file = m_options.dataDirectory / L"bar" / L"bars.json";
    std::error_code ec;
    if (plan)
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto* bytes = reinterpret_cast<const std::byte*>(plan->data());
        if (auto written = platform::writeFileAtomic(file, std::span(bytes, plan->size())); !written)
        {
            log::error("cannot write the bar plan: {}", written.error().toString());
        }
    }
    else
    {
        std::filesystem::remove(file, ec);
    }
    log::info("bar plan: {}", plan ? "set" : "none");
    notifyResident();
}

void CoreServer::setBarOff(bool off)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_barOff == off)
        {
            return;
        }
        m_barOff = off;
    }
    if (auto saved = m_state->setMeta(kBarOffMeta, off ? std::optional<std::string_view>("1") : std::nullopt);
        !saved)
    {
        log::warn("cannot save the bar switch: {}", saved.error().toString());
    }
    log::info("bar: {}", off ? "off" : "on");
    notifyResident();
}

std::optional<std::filesystem::path> CoreServer::barSource() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_barSource;
}

void CoreServer::setBarSource(const std::optional<std::filesystem::path>& folder)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_barSource == folder)
        {
            return;
        }
        m_barSource = folder;
    }
    const std::string text = folder ? utf8Path(*folder) : std::string();
    if (auto saved =
            m_state->setMeta(kBarSourceMeta, folder ? std::optional<std::string_view>(text) : std::nullopt);
        !saved)
    {
        log::warn("cannot save the bar source: {}", saved.error().toString());
    }
    notifyResident();
}

Result<void> CoreServer::reloadBars()
{
    const auto source = barSource();
    if (!source)
    {
        return {};
    }
    const std::scoped_lock lock(m_operation);
    auto loaded = loadRice(Json{{"path", utf8Path(*source)}});
    if (!loaded)
    {
        return std::unexpected(std::move(loaded.error()));
    }
    if (!loaded->rice)
    {
        // Half-typed, maybe: the bars stay as they are until the file is valid again.
        return std::unexpected(
            std::move(loaded->rice.error()).withContext("rice.toml has errors; the bars stay"));
    }
    if (loaded->rice->bars.empty())
    {
        log::info("bar: the rice has no [[bar]] anymore; apply it again (or restore) to remove the bars");
        return {};
    }
    auto plan = m_plugins->barPlan(*loaded->rice);
    if (!plan)
    {
        return std::unexpected(std::move(plan.error()));
    }
    m_plugins->setRicePlugins(RicePlugins::of(*loaded->rice)); // its plugin settings may have changed too
    bar::addAppIcons(*plan, *loaded->rice, loaded->folder);
    setBarPlan(bar::toJson(*plan));
    return {};
}

Result<void> CoreServer::startBar()
{
    if (!m_supervisor)
    {
        return fail(ErrorCode::Unsupported, "no worker supervisor");
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    platform::WorkerSpec spec;
    spec.name = std::string(kBarWorker);
    spec.executable = self->parent_path() / L"rmw-bar.exe";
    spec.arguments = L"--plan " + platform::quoteArgument((m_options.dataDirectory / L"bar" / L"bars.json").native());
    if (!m_bridgePipe.empty())
    {
        spec.arguments += L" --bridge " + platform::quoteArgument(m_bridgePipe);
    }
    spec.memoryLimitBytes = kBarMemoryLimit;
    spec.hangAfter = std::chrono::milliseconds(2000);
    spec.startGrace = std::chrono::milliseconds(10'000);
    spec.endsWithCore = false; // it shows Windows' taskbar again if the core dies
    // The apps its Start and dock open live on their own: in its job they shared its memory ceiling
    // (a browser started from "All apps" passed the limit and was killed with the bar).
    spec.childrenLeaveJob = true;
    log::info("starting the bar");
    return m_supervisor->start(std::move(spec));
}

void CoreServer::stopBar()
{
    if (m_supervisor)
    {
        m_supervisor->stop(kBarWorker);
    }
}

std::optional<std::string> CoreServer::videoPlan() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_videoPlan;
}

void CoreServer::setVideoPlan(const std::optional<std::string>& plan)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_videoPlan == plan)
        {
            return;
        }
        m_videoPlan = plan;
    }
    if (auto saved =
            m_state->setMeta(kVideoPlanMeta, plan ? std::optional<std::string_view>(*plan) : std::nullopt);
        !saved)
    {
        log::warn("cannot save the video plan: {}", saved.error().toString());
    }
    // rmw-wallpaper follows the file: rewritten, it plays the new plan; gone, it leaves.
    const auto file = m_options.dataDirectory / L"wallpaper" / L"video.json";
    std::error_code ec;
    if (plan)
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto* bytes = reinterpret_cast<const std::byte*>(plan->data());
        if (auto written = platform::writeFileAtomic(file, std::span(bytes, plan->size())); !written)
        {
            log::error("cannot write the video plan: {}", written.error().toString());
        }
    }
    else
    {
        std::filesystem::remove(file, ec);
    }
    log::info("video plan: {}", plan ? "set" : "none");
    notifyResident();
}

Result<std::optional<std::string>> CoreServer::videoPlanFor(const domain::Rice& rice,
                                                            const std::filesystem::path& folder)
{
    if (!rice.wallpaper || rice.wallpaper->layout != domain::WallpaperLayout::PerMonitor)
    {
        return std::optional<std::string>();
    }
    video::Plan plan;
    plan.policy = video::policyOf(rice.wallpaper->policy);
    plan.background = rice.wallpaper->background.value_or(domain::Color{});
    const domain::Color accent = systemAccent();
    for (const auto& rule : rice.wallpaper->monitors)
    {
        video::Entry entry;
        entry.monitor = rule.match;
        entry.fit = rule.fit;
        if (rule.live())
        {
            entry.effects = video::effectsOf(rule.effects);
            for (const auto& layer : rule.particles)
            {
                entry.particles.push_back(video::particlesOf(layer, rice, accent));
            }
            switch (rule.kind)
            {
            case domain::WallpaperKind::Video:
            case domain::WallpaperKind::Image: {
                // Into the store: the rice's folder may move or go; the plan in meta keeps the copy.
                const bool isVideo = rule.kind == domain::WallpaperKind::Video;
                auto stored = m_store->import(folder / std::filesystem::path(utf8To16Lossy(rule.source)),
                                              isVideo ? kVideoMaxBytes : limits::kImageMaxBytes);
                if (!stored)
                {
                    return std::unexpected(std::move(stored.error())
                                               .withContext(std::format("{} '{}'", isVideo ? "video" : "picture",
                                                                        rule.source)));
                }
                entry.source = isVideo ? video::Source::Video : video::Source::Image;
                entry.file = std::move(*stored);
                break;
            }
            case domain::WallpaperKind::Color:
                // The color effects change one color once: the worker only draws it.
                entry.source = video::Source::Color;
                entry.color = video::applyMatrix(video::colorMatrix(entry.effects), rule.color.value_or(domain::Color{}));
                entry.effects.brightness = 0.0;
                entry.effects.contrast = 1.0;
                entry.effects.saturation = 1.0;
                entry.effects.hue = 0.0;
                break;
            case domain::WallpaperKind::Slideshow:
                break; // never live (the parser leaves its effects out)
            }
        }
        plan.entries.push_back(std::move(entry));
    }
    if (!plan.hasLive())
    {
        return std::optional<std::string>();
    }
    return std::optional<std::string>(video::toJson(plan));
}

Result<void> CoreServer::startVideo()
{
    if (!m_supervisor)
    {
        return fail(ErrorCode::Unsupported, "no worker supervisor");
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    platform::WorkerSpec spec;
    spec.name = std::string(kVideoWorker);
    spec.executable = self->parent_path() / L"rmw-wallpaper.exe";
    spec.arguments =
        L"--plan " + platform::quoteArgument((m_options.dataDirectory / L"wallpaper" / L"video.json").native());
    spec.memoryLimitBytes = kVideoMemoryLimit;
    // docs/04 M6: a hung window thread is found in < 2 s (1.5 s of silence, plus the checks). The
    // thread beats every 250 ms and between the monitors while it builds the decoders.
    spec.hangAfter = std::chrono::milliseconds(1500);
    spec.startGrace = std::chrono::milliseconds(10'000);
    spec.endsWithCore = true; // Windows' wallpaper (the fallback image) is underneath anyway
    log::info("starting the video wallpaper");
    return m_supervisor->start(std::move(spec));
}

void CoreServer::stopVideo()
{
    if (m_supervisor)
    {
        m_supervisor->stop(kVideoWorker);
    }
}

Json CoreServer::videoStatus() const
{
    Json status{{"configured", videoPlan().has_value()}, {"running", false}};
    if (!m_supervisor)
    {
        return status;
    }
    for (const auto& worker : m_supervisor->status())
    {
        if (worker.name != kVideoWorker)
        {
            continue;
        }
        status["running"] = worker.running;
        status["pid"] = worker.pid;
        status["restarts"] = worker.restarts;
        status["disabled"] = worker.disabled;
        status["lastFailure"] = static_cast<int>(worker.lastFailure);
        if (!worker.running)
        {
            continue;
        }
        // What rmw-wallpaper says it plays on each monitor (and why not).
        auto bytes = platform::readFileLimited(m_options.dataDirectory / L"wallpaper" / L"status.json",
                                               kVideoStatusMaxBytes);
        if (!bytes)
        {
            continue;
        }
        auto played =
            json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
        if (played && played->is_object())
        {
            for (const char* key : {"adapter", "error", "monitors"})
            {
                if (played->contains(key))
                {
                    status[key] = (*played)[key];
                }
            }
        }
    }
    return status;
}

void CoreServer::setTiling(const domain::TilingSettings& settings)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        m_tiling = settings;
    }
    refreshTilerPlan();
}

void CoreServer::refreshTilerPlan()
{
    std::optional<std::string> plan;
    {
        const std::scoped_lock lock(m_scheduleMutex);
        // Tiling, or the rice's window look alone: rmw-tiler does both.
        if (m_tiling.enabled || m_windowLook)
        {
            tiling::Plan tiler;
            tiler.settings = m_tiling;
            tiler.border = m_tiling.borderColor.value_or(systemAccent());
            tiler.settings.borderColor.reset();
            tiler.look = m_windowLook;
            plan = tiling::toJson(tiler);
        }
        if (plan == m_tilerPlan)
        {
            return;
        }
        m_tilerPlan = plan;
    }
    // rmw-tiler follows the file: rewritten, it takes the new plan; gone, it puts every window back
    // and leaves.
    const auto file = m_options.dataDirectory / L"tiler" / L"plan.json";
    std::error_code ec;
    if (plan)
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        const auto* bytes = reinterpret_cast<const std::byte*>(plan->data());
        if (auto written = platform::writeFileAtomic(file, std::span(bytes, plan->size())); !written)
        {
            log::error("cannot write the tiler plan: {}", written.error().toString());
        }
    }
    else
    {
        std::filesystem::remove(file, ec);
    }
    log::info("tiler plan: {}", plan ? "set" : "none");
    notifyResident();
}

struct CoreServer::MachineJob
{
    bool restore = false;
    std::atomic<bool> done{false};
    std::atomic<DWORD> code{STILL_ACTIVE};
};

void CoreServer::setMachineIcons(bool allowed)
{
    const std::scoped_lock lock(m_scheduleMutex);
    m_machineIconsAllowed = allowed;
}

void CoreServer::collectMachineJob()
{
    std::shared_ptr<MachineJob> job;
    {
        const std::scoped_lock lock(m_scheduleMutex);
        job = m_machineJob;
    }
    if (!job || !job->done.load())
    {
        return;
    }
    // A restore that worked: nothing of ours is left on the machine. (A set that failed keeps the
    // mark: a restore later finds nothing to put back, which is harmless.)
    if (job->restore && job->code.load() == 0)
    {
        [[maybe_unused]] auto cleared = m_state->setMeta(kMachineIconsMeta, std::nullopt);
    }
    const std::scoped_lock lock(m_scheduleMutex);
    if (m_machineJob == job)
    {
        m_machineJob.reset();
    }
}

void CoreServer::runMachineHelper(const machine::Command& command)
{
    collectMachineJob();
    auto self = platform::currentExecutable();
    auto line = machine::commandLine(command);
    if (!self || !line)
    {
        log::error("machine icons: {}", !self ? self.error().toString() : line.error().toString());
        return;
    }
    auto job = std::make_shared<MachineJob>();
    job->restore = command.kind == machine::Command::Kind::RestoreDefaults;
    if (!job->restore)
    {
        // Marked now: whatever happens to this core, a later restore asks rmw-elevated to put back.
        [[maybe_unused]] auto marked = m_state->setMeta(kMachineIconsMeta, "1");
    }
    {
        const std::scoped_lock lock(m_scheduleMutex);
        m_machineJob = job;
    }
    std::thread([job, executable = self->parent_path() / L"rmw-elevated.exe", parameters = std::move(*line)] {
        try
        {
            if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
            {
                SHELLEXECUTEINFOW info{};
                info.cbSize = sizeof(info);
                info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
                info.lpVerb = L"runas";
                info.lpFile = executable.c_str();
                info.lpParameters = parameters.c_str();
                info.nShow = SW_HIDE;
                if (ShellExecuteExW(&info) != FALSE && info.hProcess != nullptr)
                {
                    WaitForSingleObject(info.hProcess, INFINITE);
                    DWORD code = 1;
                    GetExitCodeProcess(info.hProcess, &code);
                    CloseHandle(info.hProcess);
                    job->code = code;
                    log::info("machine icons: rmw-elevated {} ended ({})", job->restore ? "restore" : "set", code);
                }
                else
                {
                    log::warn("machine icons: Windows did not start rmw-elevated ({}; declined?)", GetLastError());
                }
                CoUninitialize();
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch): the job just stays not done
        {
        }
        job->done = true;
    }).detach();
}

void CoreServer::setWindowLook(const std::optional<tiling::WindowLook>& look)
{
    {
        const std::scoped_lock lock(m_scheduleMutex);
        if (m_windowLook == look)
        {
            return;
        }
        m_windowLook = look;
    }
    const std::string text = look ? tiling::toJson(*look) : std::string();
    if (auto saved = m_state->setMeta(kWindowLookMeta, look ? std::optional<std::string_view>(text) : std::nullopt);
        !saved)
    {
        log::warn("cannot save the window look: {}", saved.error().toString());
    }
    log::info("window look: {}", look ? "the rice's" : "Windows' own");
    refreshTilerPlan();
}

std::optional<std::string> CoreServer::tilerPlan() const
{
    const std::scoped_lock lock(m_scheduleMutex);
    return m_tilerPlan;
}

Result<void> CoreServer::startTiler()
{
    if (!m_supervisor)
    {
        return fail(ErrorCode::Unsupported, "no worker supervisor");
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    platform::WorkerSpec spec;
    spec.name = std::string(kTilerWorker);
    spec.executable = self->parent_path() / L"rmw-tiler.exe";
    spec.arguments = L"--plan " + platform::quoteArgument((m_options.dataDirectory / L"tiler" / L"plan.json").native());
    spec.memoryLimitBytes = kTilerMemoryLimit;
    spec.hangAfter = std::chrono::milliseconds(2000);
    spec.startGrace = std::chrono::milliseconds(10'000);
    spec.endsWithCore = false; // it puts every window back when the core is gone
    log::info("starting the tiler");
    return m_supervisor->start(std::move(spec));
}

void CoreServer::stopTiler(bool force)
{
    if (!force)
    {
        if (HWND control = FindWindowW(tiling::kControlClass, nullptr))
        {
            PostMessageW(control, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (m_supervisor)
    {
        m_supervisor->stop(kTilerWorker);
    }
    putBackTiledWindows();
}

bool CoreServer::tilerRunning() const
{
    return json::boolOr(tilerStatus(), "running", false);
}

void CoreServer::leaveTiler()
{
    if (!m_supervisor || !tilerRunning())
    {
        return;
    }
    log::info("tiler: asking it to put every window back");
    stopTiler(false);
    const auto deadline = std::chrono::steady_clock::now() + kTilerLeaveWait;
    while (tilerRunning() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(50ms);
    }
    if (tilerRunning())
    {
        log::warn("tiler: it did not leave in time; ended (its list puts the windows back)");
    }
}

Json CoreServer::tilerStatus() const
{
    Json status{{"configured", tilerPlan().has_value()}, {"running", false}};
    {
        const std::scoped_lock lock(m_scheduleMutex);
        status["tiling"] = m_tiling.enabled;
        status["windowLook"] = m_windowLook.has_value(); // rmw-tiler paints it, with tiling or alone
    }
    if (!m_supervisor)
    {
        return status;
    }
    for (const auto& worker : m_supervisor->status())
    {
        if (worker.name != kTilerWorker)
        {
            continue;
        }
        status["running"] = worker.running;
        status["pid"] = worker.pid;
        status["restarts"] = worker.restarts;
        status["disabled"] = worker.disabled;
        status["lastFailure"] = static_cast<int>(worker.lastFailure);
    }
    return status;
}

void CoreServer::putBackTiledWindows() const
{
    const auto file = m_options.dataDirectory / L"tiler" / L"windows.json";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
        return;
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return;
    }
    platform::ChildOptions options;
    options.ownJob = false; // a moment's work that must finish even if the core goes
    auto child = platform::ChildProcess::spawn(self->parent_path() / L"rmw-tiler.exe",
                                               L"--restore " + platform::quoteArgument(file.native()), options);
    if (!child)
    {
        log::error("cannot put the tiled windows back: {}", child.error().toString());
        return;
    }
    log::info("putting the tiled windows back");
    child->detach();
}

Json CoreServer::barStatus() const
{
    Json status{{"configured", barPlan().has_value()}, {"off", barOff()}, {"running", false}};
    if (!m_supervisor)
    {
        return status;
    }
    for (const auto& worker : m_supervisor->status())
    {
        if (worker.name != kBarWorker)
        {
            continue;
        }
        status["running"] = worker.running;
        status["pid"] = worker.pid;
        status["restarts"] = worker.restarts;
        status["disabled"] = worker.disabled;
        status["lastFailure"] = static_cast<int>(worker.lastFailure);
    }
    return status;
}

void CoreServer::onWorkerEvent(const platform::WorkerEvent& event)
{
    if (event.name == kPluginWorker)
    {
        onPluginHostFailure(event);
        return;
    }
    if (event.name == kTilerWorker)
    {
        // Crashed, hung or given up on: no window stays hidden until (or unless) it is back.
        if (event.kind == platform::WorkerEventKind::Failed || event.kind == platform::WorkerEventKind::Disabled)
        {
            putBackTiledWindows();
        }
        return;
    }
    if (event.name != kBarWorker || event.kind == platform::WorkerEventKind::Started)
    {
        return;
    }
    // The bar is gone — crashed, hung, leaked or left: Windows' taskbar must not stay hidden, and
    // its tray gets every app's icons again (the bar's tray window may have held some, ADR-0012).
    platform::showNativeTaskbars();
    platform::askAppsForTrayIcons();
    if (event.kind == platform::WorkerEventKind::Disabled && m_window != nullptr && m_resident != nullptr)
    {
        Resident* resident = m_resident;
        m_window->post([resident] { resident->onBarDisabled(); });
    }
}

void CoreServer::pluginStopped(const std::string& id)
{
    std::string name = id;
    if (const auto entry = m_plugins->find(id); entry && entry->manifest)
    {
        name = entry->manifest->name;
    }
    if (m_window != nullptr && m_resident != nullptr)
    {
        Resident* resident = m_resident;
        auto names = std::make_shared<const std::pair<std::string, std::string>>(std::move(name), id);
        m_window->post([resident, names] { resident->onPluginStopped(names->first, names->second); });
    }
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
    // However this ends (an exception included), the client is gone: the idle exit counts on it.
    const ScopeExit gone([this]() noexcept {
        m_lastActivity = nowTicks();
        --m_clients;
    });

    // The first message must be a compatible hello.
    auto first = connection.receive(kHelloTimeout);
    auto hello = first ? ipc::parseRequest(*first) : Result<ipc::Request>(std::unexpected(first.error()));
    if (!hello || hello->method != "hello")
    {
        return;
    }
    if (auto compatible = ipc::checkHello(hello->params); !compatible)
    {
        [[maybe_unused]] const auto refused =
            connection.send(ipc::errorText(hello->id, ipc::rpcError(ipc::RpcCode::Incompatible,
                                                                    compatible.error().message())),
                            kSendTimeout);
        return;
    }
    Json helloReply = ipc::helloParams(kVersion);
    helloReply["pid"] = GetCurrentProcessId();
    helloReply["dataDirectory"] = utf8Path(m_options.dataDirectory);
    if (!connection.send(ipc::resultText(hello->id, helloReply), kSendTimeout))
    {
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
            platform::closeSettingsWindow(); // or an open window starts the core again
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
            reply = answer(*request);
        }
        if (!request->id.is_null() && !connection.send(reply, kSendTimeout))
        {
            break;
        }
    }
}

std::string CoreServer::answer(const ipc::Request& request)
{
    // A request that throws (a path that does not convert, a bad_alloc) is answered with an error;
    // the core and this client's other requests go on.
    try
    {
        auto result = dispatch(request);
        if (result)
        {
            return ipc::resultText(request.id, *result);
        }
        if (result.error().code() == ErrorCode::Busy)
        {
            return ipc::errorText(request.id, ipc::rpcError(ipc::RpcCode::Busy, result.error().message()));
        }
        log::warn("{} failed: {}", request.method, result.error().toString());
        return ipc::errorText(request.id, ipc::toRpcError(result.error()));
    }
    catch (const std::exception& e)
    {
        log::error("{} failed: {}", request.method, e.what());
        return ipc::errorText(request.id, ipc::rpcError(ipc::RpcCode::InternalError, e.what()));
    }
    catch (...)
    {
        log::error("{} failed: unknown exception", request.method);
        return ipc::errorText(request.id, ipc::rpcError(ipc::RpcCode::InternalError, "unexpected failure"));
    }
}

Result<Json> CoreServer::exclusive(const std::function<Result<Json>()>& operation, bool changesWindows)
{
    const std::unique_lock lock(m_operation, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return fail(ErrorCode::Busy, "another operation is running; try again when it finishes");
    }
    std::optional<OperationMark> mark;
    if (changesWindows)
    {
        mark.emplace(*this);
    }
    if (m_options.testOperationDelay.count() > 0)
    {
        std::this_thread::sleep_for(m_options.testOperationDelay);
    }
    return operation();
}

Result<Json> CoreServer::perform(const domain::ActionCall& action, Origin origin)
{
    if (auto plugin = domain::parsePluginActionName(action.name))
    {
        // Not under the operation lock: the plugin may call actions of ours in turn.
        return runPluginAction(*plugin, Json::object(), origin);
    }
    Json params = Json::object();
    if (!action.ricePath.empty())
    {
        params["path"] = action.ricePath;
    }
    if (!action.app.empty())
    {
        params["app"] = action.app;
    }
    if (action.name == "launch.app")
    {
        return runAction(action.name, params, origin); // changes nothing of ours: no lock
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

Result<void> CoreServer::turnPage(const std::string& field)
{
    const std::scoped_lock lock(m_operation);
    const OperationMark mark(*this);
    return turnPageLocked(field);
}

Result<void> CoreServer::turnPageLocked(const std::string& field)
{
    Json plan = rotation();
    const auto entry = plan.find(field);
    if (entry == plan.end())
    {
        return {};
    }
    auto applied = m_state->applied("wallpaper");
    if (!applied)
    {
        return std::unexpected(std::move(applied.error()));
    }
    const auto shown = applied->find(field);
    if (shown == applied->end())
    {
        // The user chose another wallpaper there (drift): this slideshow stops for good.
        log::info("{}: changed outside; its slideshow stops", field);
        plan.erase(field);
        setRotation(plan);
        return {};
    }
    const Json items = json::memberOr(*entry, "items", Json::array());
    if (!items.is_array() || items.size() < 2 ||
        !std::ranges::all_of(items, [](const Json& item) { return item.is_string(); }))
    {
        return fail(ErrorCode::InvalidData, std::format("{}: the slideshow plan is damaged", field));
    }
    const std::string current = json::stringOr(*shown, "file", "");
    const auto at = std::ranges::find(items, Json(current));
    const auto index = static_cast<std::size_t>(at - items.begin()); // == size when not found
    const std::size_t next =
        modules::nextPage(items.size(), index, json::boolOr(*entry, "shuffle", false), m_random());
    auto report = m_engine->applyFields("wallpaper", Json{{field, Json{{"file", items[next]}}}}, "Slideshow");
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    return {};
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
    if (method == "recover")
    {
        return exclusive([&] { return recoverInterrupted(request.params); });
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
    if (method == "apps.list")
    {
        auto apps = platform::installedApps();
        if (!apps)
        {
            return std::unexpected(std::move(apps.error()));
        }
        Json list = Json::array();
        for (const auto& app : *apps)
        {
            list.push_back({{"id", app.id}, {"name", app.name}});
        }
        return list;
    }
    if (method == "module.inspect")
    {
        return exclusive([&] { return inspect(request.params); }, false);
    }
    if (method == "rice.export")
    {
        return exclusive([&] { return exportRice(request.params); }, false);
    }
    if (method == "rice.import")
    {
        return importRice(request.params);
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
    if (method.starts_with("plugin."))
    {
        return pluginMethod(request);
    }
    // action.run
    const std::string name = json::stringOr(request.params, "name", "");
    const Json actionParams = json::memberOr(request.params, "params", Json::object());
    if (auto plugin = domain::parsePluginActionName(name))
    {
        // Not under the operation lock: the plugin may call actions of ours in turn.
        return runPluginAction(*plugin, actionParams, Origin::User);
    }
    if (const std::string plugin = json::stringOr(request.params, "plugin", ""); !plugin.empty())
    {
        // From the plugin host, for one plugin: only what plugins may call, and it waits for a
        // running operation instead of failing.
        if (domain::findPluginAction(name) == nullptr)
        {
            return fail(ErrorCode::AccessDenied, std::format("plugins cannot call '{}'", name));
        }
        const Origin origin =
            json::boolOr(request.params, "byUser", false) ? Origin::User : Origin::Automatic;
        log::info("plugin {} asks for {}", plugin, name);
        const std::scoped_lock lock(m_operation);
        const OperationMark mark(*this);
        return runAction(name, actionParams, origin);
    }
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
    const std::size_t slideshows = rotation().size();
    if (const auto current = this->schedule())
    {
        schedule = {{"lightAt", timeText(current->lightAt)}, {"darkAt", timeText(current->darkAt)}};
    }
    // The folders of the rice applied last and of the one before it (what rice.undo goes back to).
    const auto metaText = [this](std::string_view key) -> Json {
        auto value = m_state->meta(key);
        return value && *value && !(*value)->empty() ? Json(**value) : Json(nullptr);
    };
    Json rice{{"current", metaText(kRiceCurrentMeta)}, {"previous", metaText(kRicePreviousMeta)}};
    // Only a look: held while the rest of the reply is built, it turned a concurrent apply away as busy.
    bool busy = false;
    {
        const std::unique_lock probe(m_operation, std::try_to_lock);
        busy = !probe.owns_lock();
    }
    Json interrupted = nullptr;
    if (auto journal = m_state->openJournal(); journal && *journal)
    {
        interrupted = Json{{"operation", (*journal)->operation}, {"startedAt", (*journal)->startedAt}};
    }
    return Json{{"version", kVersion},
                {"dataDirectory", utf8Path(m_options.dataDirectory)},
                {"originalCapturedAt", *base ? Json((*base)->createdAt) : Json(nullptr)},
                {"managedModules", *applied},
                {"rice", std::move(rice)},
                {"needsRestore", *needsRestore},
                // Safe mode leaves an interrupted operation as it is (`rmw recover`).
                {"interrupted", std::move(interrupted)},
                {"stateReplaced", m_stateMovedTo ? Json(utf8Path(*m_stateMovedTo)) : Json(nullptr)},
                {"busy", busy},
                {"safeMode", std::move(safeMode)},
                {"schedule", std::move(schedule)},
                {"slideshows", slideshows},
                {"bar", barStatus()},
                {"video", videoStatus()},
                {"tiler", tilerStatus()},
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
    if (name == "bar.on" || name == "bar.off")
    {
        setBarOff(name == "bar.off");
        return Json{{"bar", name == "bar.off" ? "off" : "on"}};
    }
    if (name == "bar.toggle")
    {
        setBarOff(!barOff());
        return Json{{"bar", barOff() ? "off" : "on"}};
    }
    if (name == "rice.undo")
    {
        // The rice before the last one comes back (applying it again swaps them: a toggle).
        auto previous = m_state->meta(kRicePreviousMeta);
        if (!previous || !*previous || (*previous)->empty())
        {
            return fail(ErrorCode::NotFound, "no rice to go back to");
        }
        return applyRice(Json{{"path", **previous}}, origin);
    }
    if (name == "launch.app")
    {
        const std::string app = json::stringOr(params, "app", "");
        if (app.empty())
        {
            return fail(ErrorCode::InvalidArgument, "launch.app needs 'app' (rmw apps lists them)");
        }
        if (auto launched = platform::launchApp(app); !launched)
        {
            return std::unexpected(std::move(launched.error()));
        }
        log::info("opened app {}", app);
        return Json{{"launched", app}};
    }
    if (name == "wallpaper.next")
    {
        const Json plan = rotation();
        if (plan.empty())
        {
            return fail(ErrorCode::NotFound, "no slideshow is running");
        }
        for (const auto& [field, entry] : plan.items())
        {
            if (auto turned = turnPageLocked(field); !turned)
            {
                return std::unexpected(std::move(turned.error()));
            }
        }
        return Json{{"turned", plan.size()}};
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
    domain::Rice rice = *loaded->rice;
    // The values of the tweak plugins it requires go through the engine like any module.
    std::vector<std::string> tweakNotes;
    rice.tweaks = m_plugins->tweaksFor(rice, tweakNotes);
    // The first apply captures "my original Windows": say if it was already customized.
    const auto base = m_state->baseSnapshot();
    const bool firstApply = base && !base->has_value();
    const std::vector<std::string> others =
        firstApply ? platform::detectOtherCustomizers() : std::vector<std::string>{};

    // Slideshows Windows cannot play are turned by the core. Their pages go into the store now: the
    // plan waits in meta while the apply runs (its clean-up must see them) and becomes the plan only
    // once the apply worked — a crash in between must not leave the core turning pages of a rice that
    // was undone.
    std::optional<Json> rotationPlan;
    if (rice.wallpaper)
    {
        auto monitors = m_logic->topology.monitors();
        if (!monitors)
        {
            return std::unexpected(std::move(monitors.error()));
        }
        engine::StoreAssetSink pages(*m_store, loaded->folder);
        auto plan = modules::wallpaperRotation(*rice.wallpaper, *monitors, pages);
        if (!plan)
        {
            return std::unexpected(std::move(plan.error()));
        }
        rotationPlan = std::move(*plan);
        if (auto pending = m_state->setMeta(kRotationPendingMeta, json::dump(*rotationPlan)); !pending)
        {
            log::warn("cannot keep the slideshow plan: {}", pending.error().toString());
        }
    }
    const auto dropPending = [&] {
        if (rotationPlan)
        {
            [[maybe_unused]] auto dropped = m_state->setMeta(kRotationPendingMeta, std::nullopt);
        }
    };

    // Ricing Manager's bars and the window look are the core's, not a module's: a rice of bars (or
    // [windows]) only still does something.
    const bool bars = !rice.bars.empty() || (rice.windows && !rice.windows->empty()) ||
                      (rice.icons && !rice.icons->machine.empty());
    auto report = origin == Origin::User
                      ? m_engine->apply(rice, loaded->folder, bars)
                      : m_engine->applyAutomatic(rice, loaded->folder,
                                                 std::format("Automatic: {}", rice.meta.name), bars);
    if (!report)
    {
        dropPending();
        return std::unexpected(std::move(report.error()));
    }
    if (rotationPlan)
    {
        setRotation(*rotationPlan);
        dropPending();
    }
    for (auto& note : tweakNotes)
    {
        report->warnings.push_back(std::move(note));
    }
    // What the rice asks of plugins; and which rice to go back to (rice.undo).
    m_plugins->setRicePlugins(RicePlugins::of(rice));
    const std::string folderText = utf8Path(loaded->folder);
    if (auto current = m_state->meta(kRiceCurrentMeta); current && *current && **current != folderText)
    {
        [[maybe_unused]] auto saved = m_state->setMeta(kRicePreviousMeta, **current);
    }
    [[maybe_unused]] auto savedCurrent = m_state->setMeta(kRiceCurrentMeta, folderText);
    if (rice.windows)
    {
        // Other apps' windows: rmw-tiler paints them (a rice without [windows] leaves the look as is).
        setWindowLook(tiling::lookFromRice(*rice.windows, rice.palette, systemAccent()));
    }
    bool machineAsked = false;
    if (rice.icons && !rice.icons->machine.empty())
    {
        bool allowed = false;
        {
            const std::scoped_lock lock(m_scheduleMutex);
            allowed = m_machineIconsAllowed;
        }
        if (!allowed)
        {
            report->warnings.emplace_back("icons: the folder and drive icons of the whole PC are an advanced option: "
                                          "turn it on in Settings (it asks for administrator)");
        }
        else if (auto command = machineCommand(rice, loaded->folder, *m_store); !command)
        {
            report->warnings.push_back(std::format("icons: {}", command.error().message()));
        }
        else
        {
            runMachineHelper(*command);
            machineAsked = true;
            report->warnings.emplace_back(
                "icons: Windows asks for administrator to change the folder and drive icons of the whole PC");
        }
    }
    if (!rice.bars.empty())
    {
        // The custom taskbar: rmw-bar shows this plan (palette resolved, skins applied, plugin
        // widgets found), and follows the rice's file as its author edits it.
        if (auto plan = m_plugins->barPlan(rice))
        {
            bar::addAppIcons(*plan, rice, loaded->folder);
            setBarPlan(bar::toJson(*plan));
            setBarSource(loaded->folder);
        }
        else
        {
            report->warnings.push_back(std::format("bar: {}", plan.error().message()));
        }
    }
    if (rice.wallpaper)
    {
        // Video wallpapers: rmw-wallpaper plays them over the fallback images the engine set
        // (copied into the store after the operation, whose clean-up would not know them yet).
        if (auto plan = videoPlanFor(rice, loaded->folder))
        {
            setVideoPlan(*plan);
        }
        else
        {
            report->warnings.push_back(std::format("video: {}", plan.error().message()));
        }
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
    result["bars"] = rice.bars.size();
    result["windowLook"] = rice.windows && !rice.windows->empty();
    result["machineIcons"] = machineAsked;
    return result;
}

Result<Json> CoreServer::setAppearance(std::string_view action, Origin origin)
{
    domain::ThemeMode mode = action == "appearance.dark" ? domain::ThemeMode::Dark : domain::ThemeMode::Light;
    if (action == "appearance.toggle")
    {
        // Flip whatever Windows shows now.
        auto now = runner().capture("appearance", {});
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
    // The original Windows: what is ours goes even when none of Windows' settings could come back.
    if (!report && !original)
    {
        return std::unexpected(std::move(report.error()));
    }
    bool hadLook = false;
    {
        const std::scoped_lock lock(m_scheduleMutex);
        hadLook = m_windowLook.has_value();
    }
    // A history entry brings back what the core had then (bars, live wallpaper, schedule...); the
    // original Windows, or an entry older than v1.8 that did not keep it, ends all of that.
    const Json then = report && !original ? report->core : Json::object();
    bringBack(then);
    collectMachineJob();
    bool machineAsked = false;
    if (auto marked = m_state->meta(kMachineIconsMeta);
        marked && *marked && !json::boolOr(then, "machineIcons", false))
    {
        runMachineHelper(machine::Command{machine::Command::Kind::RestoreDefaults, {}});
        machineAsked = true;
        if (report)
        {
            report->warnings.emplace_back(
                "icons: Windows asks for administrator to bring back the folder and drive icons of the whole PC");
        }
    }
    notifyResident();
    if (!report)
    {
        return std::unexpected(std::move(report.error()));
    }
    Json result = reportJson(*report);
    {
        const std::scoped_lock lock(m_scheduleMutex);
        result["windowLook"] = hadLook && !m_windowLook; // taken off other apps' windows
    }
    result["machineIcons"] = machineAsked;
    return result;
}

Result<Json> CoreServer::recoverInterrupted(const Json& params)
{
    const bool discard = json::boolOr(params, "discard", false);
    auto done = discard ? m_engine->discardJournal() : m_engine->recover();
    if (!done)
    {
        return std::unexpected(std::move(done.error()));
    }
    if (*done)
    {
        // An undone apply may have left its slideshow plan waiting.
        [[maybe_unused]] auto dropped = m_state->setMeta(kRotationPendingMeta, std::nullopt);
    }
    return Json{{"interrupted", *done},
                {"discarded", discard && *done},
                {"needsRestore", m_engine->needsRestore().value_or(false)}};
}

Json CoreServer::coreState() const
{
    Json core = Json::object();
    {
        const std::scoped_lock lock(m_scheduleMutex);
        const auto text = [](const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); };
        core["schedule"] = m_schedule ? Json(scheduleText(*m_schedule)) : Json(nullptr);
        core["rotation"] = m_rotation;
        core["barPlan"] = text(m_barPlan);
        core["barSource"] = m_barSource ? Json(utf8Path(*m_barSource)) : Json(nullptr);
        core["videoPlan"] = text(m_videoPlan);
        core["windowLook"] = m_windowLook ? Json(tiling::toJson(*m_windowLook)) : Json(nullptr);
    }
    auto current = m_state->meta(kRiceCurrentMeta);
    core["riceCurrent"] = current && *current ? Json(**current) : Json(nullptr);
    auto machine = m_state->meta(kMachineIconsMeta);
    core["machineIcons"] = machine && machine->has_value();
    return core;
}

void CoreServer::bringBack(const Json& core)
{
    const auto text = [&](std::string_view key) -> std::optional<std::string> {
        const Json value = json::memberOr(core, key, Json());
        return value.is_string() ? std::optional<std::string>(value.get<std::string>()) : std::nullopt;
    };
    const auto schedule = text("schedule");
    setSchedule(schedule ? parseScheduleText(*schedule) : std::nullopt);
    setRotation(json::memberOr(core, "rotation", Json::object()));
    setBarPlan(text("barPlan"));
    std::optional<std::filesystem::path> source;
    if (const auto saved = text("barSource"))
    {
        if (auto wide = utf8To16(*saved))
        {
            source = std::filesystem::path(*wide);
        }
    }
    setBarSource(source);
    setVideoPlan(text("videoPlan"));
    std::optional<tiling::WindowLook> look;
    if (const auto saved = text("windowLook"))
    {
        if (auto parsed = tiling::parseLook(*saved))
        {
            look = std::move(*parsed);
        }
    }
    setWindowLook(look);
    // What that rice asked of plugins, read again from its folder (gone: none).
    const auto riceFolder = text("riceCurrent");
    RicePlugins plugins;
    if (riceFolder)
    {
        if (auto loaded = loadRice(Json{{"path", *riceFolder}}); loaded && loaded->rice)
        {
            plugins = RicePlugins::of(*loaded->rice);
        }
    }
    m_plugins->setRicePlugins(plugins);
    [[maybe_unused]] auto saved = m_state->setMeta(
        kRiceCurrentMeta, riceFolder ? std::optional<std::string_view>(*riceFolder) : std::nullopt);
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
    auto captured = runner().capture(module, {});
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

Result<Json> CoreServer::importRice(const Json& params) const
{
    auto archive = utf8To16(json::stringOr(params, "path", ""));
    if (!archive || archive->empty())
    {
        return fail(ErrorCode::InvalidArgument, "missing 'path' (the .rice file)");
    }
    auto unpacked =
        unpackInWorker(m_options.dataDirectory, std::filesystem::path(*archive), package::Kind::Rice);
    if (!unpacked)
    {
        return std::unexpected(std::move(unpacked.error()));
    }
    auto placed = placeRice(m_options.dataDirectory, *unpacked);
    if (!placed)
    {
        return std::unexpected(std::move(placed.error()));
    }
    return Json{{"path", utf16To8Lossy(placed->folder.native())},
                {"name", placed->name},
                {"files", unpacked->files},
                {"bytes", unpacked->bytes}};
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
          std::pair{"cursors", &look.cursors}, std::pair{"sounds", &look.sounds},
          std::pair{"icons", &look.icons}, std::pair{"lockscreen", &look.lockscreen},
          std::pair{"wallpaper", &look.wallpaper}})
    {
        auto captured = runner().capture(module, {});
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

// ---- plugins (docs/12) ----------------------------------------------------------------------------

Result<Json> CoreServer::hostCall(std::string_view method, const Json& params,
                                  std::chrono::milliseconds timeout)
{
    auto self = platform::currentExecutable();
    if (!self || m_controlPipe.empty())
    {
        return fail(ErrorCode::Unsupported, "plugins are not available");
    }
    auto connection = platform::connectPipe(m_controlPipe, 2s, self->parent_path() / L"rmw-plugin-host.exe");
    if (!connection)
    {
        return fail(ErrorCode::Unsupported, "the plugin host is not running (no plugin with a script is on)");
    }
    if (auto sent = connection->send(ipc::requestText(1, method, params), kSendTimeout); !sent)
    {
        return std::unexpected(std::move(sent.error()));
    }
    auto text = connection->receive(timeout);
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

Result<Json> CoreServer::runPluginAction(const domain::PluginActionName& action, const Json& args,
                                         Origin origin)
{
    log::info("action {}:{}", action.plugin, action.action);
    return hostCall("plugin.action",
                    Json{{"plugin", action.plugin},
                         {"action", action.action},
                         {"args", args.is_object() ? args : Json::object()},
                         {"byUser", origin == Origin::User}},
                    15s);
}

void CoreServer::pluginsChanged()
{
    // The bars show plugin widgets from the plan: read the rice again for new folders or settings.
    if (barSource())
    {
        if (auto reloaded = reloadBars(); !reloaded)
        {
            log::warn("bar: {}", reloaded.error().toString());
        }
    }
    notifyResident();
}

Result<Json> CoreServer::pluginMethod(const ipc::Request& request)
{
    const std::string& method = request.method;
    const std::string id = json::stringOr(request.params, "id", "");
    if (method == "plugin.list")
    {
        Json running = Json::object();
        if (auto status = hostCall("status", Json::object(), 5s))
        {
            for (const auto& plugin : json::memberOr(*status, "plugins", Json::array()))
            {
                running[json::stringOr(plugin, "id", "")] = plugin;
            }
            for (const auto& [plugin, reason] : json::memberOr(*status, "errors", Json::object()).items())
            {
                running[plugin]["error"] = reason;
            }
        }
        Json list = Json::array();
        for (const auto& entry : m_plugins->list())
        {
            Json item = entry.toJson();
            if (const auto found = running.find(entry.id); found != running.end())
            {
                item["host"] = *found;
            }
            list.push_back(std::move(item));
        }
        return list;
    }
    if (method == "plugin.status")
    {
        return hostCall("status", Json::object(), 5s);
    }
    if (method == "plugin.report")
    {
        // From the host: a plugin failed too often in it (errors, time, memory).
        const std::string plugin = json::stringOr(request.params, "plugin", "");
        if (!domain::isValidPackageId(plugin))
        {
            return fail(ErrorCode::InvalidArgument, "invalid plugin id");
        }
        m_plugins->disable(plugin, json::stringOr(request.params, "reason", "failed too often"));
        pluginStopped(plugin);
        return Json{{"ok", true}};
    }
    Result<PluginEntry> entry = fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    if (method == "plugin.approve")
    {
        entry = m_plugins->approve(id);
    }
    else if (method == "plugin.enable" || method == "plugin.disable")
    {
        entry = m_plugins->setEnabled(id, method == "plugin.enable");
    }
    else if (method == "plugin.dev")
    {
        if (json::boolOr(request.params, "stop", false))
        {
            if (auto removed = m_plugins->removeDev(id); !removed)
            {
                return std::unexpected(std::move(removed.error()));
            }
            pluginsChanged();
            return Json{{"ok", true}};
        }
        auto folder = utf8To16(json::stringOr(request.params, "path", ""));
        if (!folder || folder->empty())
        {
            return fail(ErrorCode::InvalidArgument, "missing 'path' (the plugin's folder)");
        }
        entry = m_plugins->addDev(std::filesystem::path(*folder));
        // The developer saw the permissions (the CLI shows them and asks): approve them.
        if (entry && entry->needsApproval() && json::boolOr(request.params, "approve", false))
        {
            entry = m_plugins->approve(entry->id);
        }
    }
    else if (method == "plugin.install")
    {
        auto source = utf8To16(json::stringOr(request.params, "path", ""));
        if (!source || source->empty())
        {
            return fail(ErrorCode::InvalidArgument, "missing 'path' (the plugin's folder or .rmwp)");
        }
        std::filesystem::path folder(*source);
        std::error_code ec;
        std::optional<core::Unpacked> unpacked;
        if (std::filesystem::is_regular_file(folder, ec))
        {
            // A .rmwp: unpacked and checked by the import worker first (docs/06 §4).
            auto worker = core::unpackInWorker(m_options.dataDirectory, folder, package::Kind::Plugin);
            if (!worker)
            {
                return std::unexpected(std::move(worker.error()));
            }
            unpacked = std::move(*worker);
            folder = unpacked->folder;
        }
        entry = m_plugins->install(folder);
        if (unpacked)
        {
            std::filesystem::remove_all(unpacked->folder, ec);
        }
        if (entry && entry->needsApproval() && json::boolOr(request.params, "approve", false))
        {
            entry = m_plugins->approve(entry->id);
        }
    }
    else if (method == "plugin.remove")
    {
        if (auto removed = m_plugins->remove(id); !removed)
        {
            return std::unexpected(std::move(removed.error()));
        }
        pluginsChanged();
        return Json{{"ok", true}};
    }
    if (!entry)
    {
        return std::unexpected(std::move(entry.error()));
    }
    pluginsChanged();
    return entry->toJson();
}

Result<void> CoreServer::startPluginHost()
{
    if (!m_supervisor)
    {
        return fail(ErrorCode::Unsupported, "no worker supervisor");
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    platform::WorkerSpec spec;
    spec.name = std::string(kPluginWorker);
    spec.executable = self->parent_path() / L"rmw-plugin-host.exe";
    spec.arguments = std::format(L"--data {} --bridge {} --control {}",
                                 platform::quoteArgument(m_options.dataDirectory.native()),
                                 platform::quoteArgument(m_bridgePipe), platform::quoteArgument(m_controlPipe));
    if (m_blame != nullptr)
    {
        spec.arguments += std::format(L" --blame {}", reinterpret_cast<std::uintptr_t>(m_blame));
        spec.inheritHandles.push_back(m_blame);
    }
    spec.memoryLimitBytes = kPluginHostMemoryLimit;
    spec.hangAfter = std::chrono::milliseconds(3000);
    spec.startGrace = std::chrono::milliseconds(10'000);
    spec.endsWithCore = true;
    // Plugin code never starts programs. No UI restrictions on the job: any of them hides the other
    // processes' windows, and windows.list needs them (the Luau sandbox is the boundary there).
    spec.noChildProcesses = true;
    log::info("starting the plugin host");
    return m_supervisor->start(std::move(spec));
}

void CoreServer::syncPluginHost()
{
    plugins::HostPlan plan;
    if (m_safeMode == SafeModeReason::None && m_plugins)
    {
        std::optional<bar::Plan> bars;
        if (const auto text = barPlan(); text && !barOff())
        {
            if (auto parsed = bar::planFromJson(*text))
            {
                bars = std::move(*parsed);
            }
        }
        plan = m_plugins->hostPlan(bars);
    }
    m_pluginsWanted = !plan.plugins.empty();
    const std::string text = plugins::toJson(plan);
    const auto file = m_options.dataDirectory / L"plugin-host" / L"plan.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (auto current = platform::readFileLimited(file, std::uint64_t{1} << 20);
        !current || std::string_view(reinterpret_cast<const char*>(current->data()), current->size()) != text)
    {
        if (auto written = platform::writeFileAtomic(file, std::as_bytes(std::span(text))); !written)
        {
            log::error("plugins: cannot write the host's plan: {}", written.error().toString());
        }
    }
    if (!m_supervisor)
    {
        return;
    }
    bool running = false;
    for (const auto& worker : m_supervisor->status())
    {
        if (worker.name == kPluginWorker)
        {
            running = worker.running || (!worker.disabled && worker.restarts > 0);
        }
    }
    if (plan.plugins.empty())
    {
        if (m_hostPlan)
        {
            [[maybe_unused]] auto stopped = hostCall("stop", Json::object(), 2s); // saves plugin storage
            m_supervisor->stop(kPluginWorker);
            m_hostPlan.reset();
        }
        return;
    }
    // A running host follows the plan file itself; a new plan restarts one that gave up.
    if (m_hostPlan && (running || *m_hostPlan == text))
    {
        m_hostPlan = text;
        return;
    }
    if (auto started = startPluginHost(); !started)
    {
        log::error("the plugin host did not start: {}", started.error().toString());
    }
    m_hostPlan = text;
}

void CoreServer::onPluginHostFailure(const platform::WorkerEvent& event)
{
    if (event.kind != platform::WorkerEventKind::Failed || m_blame == nullptr)
    {
        return;
    }
    std::string blamed;
    if (auto* record = static_cast<plugins::BlameRecord*>(
            MapViewOfFile(m_blame, FILE_MAP_WRITE, 0, 0, sizeof(plugins::BlameRecord))))
    {
        blamed.assign(record->plugin.data(), strnlen(record->plugin.data(), record->plugin.size()));
        record->plugin[0] = '\0';
        UnmapViewOfFile(record);
    }
    if (!domain::isValidPackageId(blamed))
    {
        log::warn("the plugin host ended (failure {}) with no plugin running",
                  static_cast<int>(event.failure));
        return;
    }
    log::warn("the plugin host ended (failure {}) while {} ran", static_cast<int>(event.failure), blamed);
    const auto now = std::chrono::steady_clock::now();
    bool disable = false;
    {
        const std::scoped_lock lock(m_blameMutex);
        auto& times = m_blames[blamed];
        times.push_back(now);
        while (!times.empty() && now - times.front() > kBlameWindow)
        {
            times.pop_front();
        }
        disable = static_cast<int>(times.size()) >= kBlamesToDisable;
        if (disable)
        {
            times.clear();
        }
    }
    if (disable)
    {
        m_plugins->disable(blamed, "it brought the plugin host down twice (hung or out of memory)");
        pluginStopped(blamed); // and the next host runs without it
    }
}

} // namespace rmw::core
