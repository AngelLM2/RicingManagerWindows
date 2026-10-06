#include "Resident.hpp"

#include "JsonReports.hpp"
#include "Strings.hpp"
#include "WorkQueue.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/platform/Autostart.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/NativeTaskbar.hpp"
#include "rmw/platform/Pipe.hpp"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <utility>

namespace rmw::core
{
namespace
{

using ipc::Json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

enum TimerId : std::uint32_t
{
    kIdleTimer = 1,
    kHealthyTimer,
    kScheduleTimer,
    kDriftTimer,
    kDisplayTimer,
    kExplorerTimer,
    kSettingsTimer,
    kTrayPromoteTimer,
    kPagesTimer,
    kBarSourceTimer,
    kTilerStopTimer,
};

enum TrayId : std::uint32_t
{
    kTrayToggle = 100,
    kTrayRestore,
    kTrayReload,
    kTrayExit,
    kTraySafeMode,
    kTrayBar,
    kTrayOpen,
    kTrayAutostart,
};

// Always the same, documented chord: brings Windows' taskbar back whatever the bar does.
constexpr std::uint32_t kEmergencyHotkey = 0xBF00;
constexpr std::uint32_t kEmergencyModifiers = 0x2 | 0x1 | 0x8; // Ctrl+Alt+Win
constexpr std::uint32_t kEmergencyKey = 'T';

constexpr std::uint32_t kConfigWatch = 1;
constexpr std::uint32_t kRiceWatch = 2;

constexpr auto kIdleCheck = 5s;
constexpr auto kHealthyAfter = 60s; // docs/06 §6: a start that lasts this long was fine
constexpr auto kDriftDebounce = 1500ms;
constexpr auto kDisplaySettle = 2s;  // geometry read too early is wrong (docs/03 §9)
constexpr auto kExplorerSettle = 3s; // let the new Explorer build its windows first
constexpr auto kSettingsDebounce = 500ms;
constexpr auto kScheduleMargin = 1500ms; // fire just after the minute starts, never before
constexpr auto kScheduleMaxWait = 1h;    // re-check the clock at least hourly
constexpr std::int32_t kDueWindowSeconds = 120;
constexpr auto kFiredGuard = 150s;
// rmw-tiler puts every window back and leaves when asked; past this it is ended and the core does it.
constexpr auto kTilerStopWait = 3s;

std::int32_t localSecondsOfDay() noexcept
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return (now.wHour * 3600) + (now.wMinute * 60) + now.wSecond;
}

std::optional<std::filesystem::file_time_type> stampOf(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec)
    {
        return std::nullopt;
    }
    return time;
}

std::wstring_view displayName(std::string_view module) noexcept
{
    const UiStrings& text = uiStrings();
    if (module == "appearance")
    {
        return text.appearance;
    }
    if (module == "cursors")
    {
        return text.cursors;
    }
    if (module == "taskbar")
    {
        return text.taskbar;
    }
    return text.wallpaper;
}

std::wstring formatted(std::wstring_view pattern, std::wstring_view first)
{
    return std::vformat(pattern, std::make_wformat_args(first));
}

std::wstring formatted(std::wstring_view pattern, std::wstring_view first, std::wstring_view second)
{
    return std::vformat(pattern, std::make_wformat_args(first, second));
}

} // namespace

std::filesystem::path settingsFile(const std::filesystem::path& dataDirectory)
{
    return dataDirectory / L"config" / L"settings.toml";
}

LoadedSettings loadSettings(const std::filesystem::path& dataDirectory)
{
    LoadedSettings loaded;
    const auto file = settingsFile(dataDirectory);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
        return loaded;
    }
    loaded.present = true;
    auto bytes = platform::readFileLimited(file, limits::kManifestMaxBytes);
    if (!bytes)
    {
        loaded.valid = false;
        loaded.error = bytes.error().message();
        return loaded;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    auto parsed = domain::parseSettings(text, loaded.diagnostics, kActions);
    if (!parsed)
    {
        loaded.valid = false;
        loaded.error = parsed.error().message();
        return loaded;
    }
    loaded.settings = std::move(*parsed);
    return loaded;
}

Resident::Resident(CoreServer& core, platform::MessageWindow& window, WorkQueue& work)
    : m_core(&core)
    , m_window(&window)
    , m_work(&work)
{
}

void Resident::start()
{
    m_started = Clock::now();
    m_lastWallClock = std::chrono::system_clock::now();

    const auto file = settingsFile(m_core->dataDirectory());
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (auto watched = m_window->watchFolder(kConfigWatch, file.parent_path()); !watched)
    {
        log::warn("settings.toml changes will need `rmw settings reload`: {}", watched.error().toString());
    }
    [[maybe_unused]] const Json first = reloadSettings();
    if (safe() || !m_core->tilerPlan())
    {
        m_core->putBackTiledWindows(); // after a crash of everything, no window stays hidden
    }
    refresh(); // also when settings.toml is broken: tray and idle exit still follow
    m_window->setTimer(kHealthyTimer, kHealthyAfter);

    if (safe())
    {
        const UiStrings& text = uiStrings();
        notify(text.safeModeTitle,
               formatted(text.safeModeText, m_core->safeMode() == SafeModeReason::Requested
                                                ? text.safeModeRequested
                                                : text.safeModeCrashes));
        return;
    }
    // Monitors present now; rules fire for the ones that arrive later.
    m_work->push([this] {
        auto keys = m_core->monitorKeys();
        if (keys)
        {
            m_window->post([this, list = std::move(*keys)]() mutable { onMonitors(std::move(list)); });
        }
    });
    // The schedule may have switched while the core was not running.
    queueScheduleSync();
}

void Resident::onEvent(const platform::SystemEvent& event)
{
    using platform::SystemEventKind;
    switch (event.kind)
    {
    case SystemEventKind::Timer:
        onTimer(event.id);
        return;
    case SystemEventKind::TrayCommand:
        onTray(event.id);
        return;
    case SystemEventKind::Hotkey:
        onHotkey(event.id);
        return;
    case SystemEventKind::FolderChanged:
        // Editors save in bursts (temporary file, rename): read once they are done.
        m_window->setTimer(event.id == kRiceWatch ? kBarSourceTimer : kSettingsTimer, kSettingsDebounce);
        return;
    case SystemEventKind::SessionEnding:
        // Log off or shut down kills us next: that is not a crash.
        m_core->markHealthy();
        return;
    default:
        break;
    }
    if (safe())
    {
        return; // nothing automatic in safe mode
    }
    switch (event.kind)
    {
    case SystemEventKind::ThemeChanged:
        onChanged("appearance");
        m_core->refreshTilerPlan(); // the border follows the accent
        break;
    case SystemEventKind::CursorsChanged:
        onChanged("cursors");
        break;
    case SystemEventKind::WallpaperChanged:
        onChanged("wallpaper");
        break;
    case SystemEventKind::TaskbarChanged:
        onChanged("taskbar");
        break;
    case SystemEventKind::ExplorerRestarted:
        log::info("Explorer restarted");
        m_window->setTimer(kExplorerTimer, kExplorerSettle);
        break;
    case SystemEventKind::DisplayChanged:
    case SystemEventKind::MonitorArrived:
        // Windows may move wallpapers around while monitors settle: that is not the user.
        m_displaySettlingUntil = Clock::now() + kDisplaySettle + 4s;
        m_window->setTimer(kDisplayTimer, kDisplaySettle);
        break;
    case SystemEventKind::PowerSource:
        onPowerSource(event.onAc);
        break;
    case SystemEventKind::TimeChanged:
    case SystemEventKind::Resumed:
        onClockJump();
        break;
    default:
        break;
    }
}

void Resident::onTimer(std::uint32_t id)
{
    switch (id)
    {
    case kIdleTimer:
        if (!m_resident && m_work->idle() && m_core->idleExpired())
        {
            log::info("idle: exiting");
            m_window->quit(0);
            return;
        }
        m_window->setTimer(kIdleTimer, kIdleCheck);
        return;
    case kHealthyTimer:
        m_core->markHealthy();
        return;
    case kScheduleTimer:
        onScheduleTimer();
        return;
    case kDriftTimer:
        checkDrift();
        return;
    case kDisplayTimer:
        onDisplaySettled();
        return;
    case kExplorerTimer:
        m_work->push([this] {
            if (auto reapplied = m_core->reapply({}); !reapplied)
            {
                log::warn("reapply after the Explorer restart: {}", reapplied.error().toString());
            }
        });
        return;
    case kPagesTimer:
        onPagesTimer();
        return;
    case kBarSourceTimer:
        m_work->push([this] {
            if (auto reloaded = m_core->reloadBars(); !reloaded)
            {
                log::warn("bar: {}", reloaded.error().toString());
            }
        });
        return;
    case kTrayPromoteTimer:
        promoteTrayIconOnce();
        return;
    case kTilerStopTimer:
        if (!m_tilerStartedFor && m_core->tilerRunning())
        {
            log::warn("tiler: it did not leave when asked; ended");
            m_core->stopTiler(true);
        }
        return;
    case kSettingsTimer:
        if (stampOf(settingsFile(m_core->dataDirectory())) != m_settingsStamp)
        {
            [[maybe_unused]] const Json reloaded = reloadSettings();
        }
        return;
    default:
        return;
    }
}

void Resident::onTray(std::uint32_t id)
{
    switch (id)
    {
    case kTrayToggle:
        queueAction({.name = "appearance.toggle", .ricePath = {}, .app = {}}, Origin::User);
        return;
    case kTrayRestore:
        queueAction({.name = "restore.original", .ricePath = {}, .app = {}}, Origin::User);
        return;
    case kTrayReload: {
        [[maybe_unused]] const Json reloaded = reloadSettings();
        return;
    }
    case kTrayBar:
        queueAction({.name = m_core->barOff() ? "bar.on" : "bar.off", .ricePath = {}, .app = {}},
                    Origin::User);
        return;
    case kTrayExit:
        log::info("exit from the tray");
        m_window->quit(0);
        return;
    case kTrayOpen:
        openSettings();
        return;
    case kTrayAutostart: {
        auto self = platform::currentExecutable();
        const bool enable = !startsWithWindows();
        if (auto set =
                self ? platform::setAutostart(enable, *self) : Result<void>(std::unexpected(self.error()));
            !set)
        {
            log::warn("start with Windows: {}", set.error().toString());
            return;
        }
        log::info("start with Windows: {} (tray)", enable ? "on" : "off");
        showTray(); // the check mark follows
        return;
    }
    default:
        return;
    }
}

void Resident::promoteTrayIconOnce()
{
    // Windows 11 puts a new tray icon behind the "^"; the user asked to see ours. Once: Explorer's
    // per-user entry for our executable gets IsPromoted = 1, and a flag file says it was done, so
    // hiding it again in Windows' settings sticks.
    const auto flag = m_core->dataDirectory() / L"config" / L"tray-promoted";
    std::error_code ec;
    if (std::filesystem::exists(flag, ec))
    {
        return;
    }
    auto self = platform::currentExecutable();
    if (!self)
    {
        return;
    }
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LR"(Control Panel\NotifyIconSettings)", 0, KEY_READ | KEY_SET_VALUE,
                      &root) != ERROR_SUCCESS)
    {
        return; // not Windows 11
    }
    bool promoted = false;
    std::array<wchar_t, 64> name{};
    for (DWORD i = 0;; ++i)
    {
        auto length = static_cast<DWORD>(name.size());
        if (RegEnumKeyExW(root, i, name.data(), &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        std::wstring path(MAX_PATH * 2, L'\0');
        auto bytes = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        if (RegGetValueW(root, name.data(), L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path.data(), &bytes) !=
            ERROR_SUCCESS)
        {
            continue;
        }
        path.resize(wcsnlen(path.c_str(), path.size()));
        if (CompareStringOrdinal(path.c_str(), -1, self->c_str(), -1, TRUE) != CSTR_EQUAL)
        {
            continue;
        }
        const DWORD one = 1;
        promoted =
            RegSetKeyValueW(root, name.data(), L"IsPromoted", REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS;
        break;
    }
    RegCloseKey(root);
    if (!promoted)
    {
        return; // Explorer has not listed it yet: next time the tray shows
    }
    std::filesystem::create_directories(flag.parent_path(), ec);
    if (auto written = platform::writeFileAtomic(flag, std::as_bytes(std::span("1", 1))); written)
    {
        log::info("tray: our icon is shown next to the clock (once; Windows' settings can hide it)");
    }
}

bool Resident::startsWithWindows()
{
    auto self = platform::currentExecutable();
    auto state = self ? platform::autostartState(*self)
                      : Result<platform::AutostartState>(std::unexpected(self.error()));
    return state && state->registered && state->current && !state->disabledByUser;
}

void Resident::openSettings()
{
    // rmw-settings.exe sits next to the core; a second one brings the first to the front.
    auto self = platform::currentExecutable();
    if (!self)
    {
        return;
    }
    const auto settings = self->parent_path() / L"rmw-settings.exe";
    const std::wstring arguments = std::format(L"--data-dir \"{}\"", m_core->dataDirectory().native());
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", settings.c_str(), arguments.c_str(),
                                                nullptr, SW_SHOWNORMAL)) <= 32)
    {
        log::warn("tray: cannot open {}", utf16To8Lossy(settings.native()));
    }
}

void Resident::onHotkey(std::uint32_t id)
{
    if (id == kEmergencyHotkey)
    {
        emergency();
        return;
    }
    const auto& hotkeys = m_loaded.settings.hotkeys;
    if (id == 0 || id > hotkeys.size())
    {
        return;
    }
    const auto& hotkey = hotkeys[id - 1];
    log::info("hotkey {}: {}", hotkey.keys.toString(), hotkey.action.name);
    queueAction(hotkey.action, Origin::User);
}

void Resident::onChanged(const std::string& module)
{
    if (m_core->ownChangesSettling())
    {
        return; // our own operation's broadcast
    }
    if (module == "wallpaper" && Clock::now() < m_displaySettlingUntil)
    {
        return;
    }
    if (std::ranges::find(m_dirty, module) == m_dirty.end())
    {
        m_dirty.push_back(module);
    }
    m_window->setTimer(kDriftTimer, kDriftDebounce); // restarts the debounce
}

void Resident::checkDrift()
{
    auto modules = std::exchange(m_dirty, {});
    if (modules.empty() || m_core->ownChangesSettling())
    {
        return;
    }
    const bool scheduled = m_core->schedule().has_value();
    m_work->push([this, modules = std::move(modules), scheduled] {
        for (const auto& module : modules)
        {
            auto detached = m_core->detachDrifted(module);
            if (!detached)
            {
                log::warn("drift check for {}: {}", module, detached.error().toString());
                continue;
            }
            if (detached->empty())
            {
                continue;
            }
            // With a schedule, switching light/dark by hand is expected: it lasts until the next
            // switch, without a notification.
            const bool modeOnly = std::ranges::all_of(*detached, [](const std::string& field) {
                return field == "appsLight" || field == "systemLight";
            });
            if (module == "appearance" && scheduled && modeOnly)
            {
                continue;
            }
            m_window->post([this, name = module] {
                const UiStrings& text = uiStrings();
                notify(text.driftTitle, formatted(text.driftText, displayName(name)));
            });
        }
    });
}

void Resident::onDisplaySettled()
{
    m_work->push([this] {
        // A monitor that came back gets its wallpaper again (docs/03 §5.2).
        if (auto reapplied = m_core->reapply({"wallpaper"}); !reapplied)
        {
            log::warn("reapply after a display change: {}", reapplied.error().toString());
        }
        auto keys = m_core->monitorKeys();
        if (!keys)
        {
            log::warn("monitors: {}", keys.error().toString());
            return;
        }
        m_window->post([this, list = std::move(*keys)]() mutable { onMonitors(std::move(list)); });
    });
}

void Resident::onMonitors(std::vector<std::string> keys)
{
    if (!m_monitorsKnown)
    {
        m_monitors = std::move(keys);
        m_monitorsKnown = true;
        return;
    }
    std::vector<std::string> arrived;
    for (const auto& key : keys)
    {
        if (std::ranges::find(m_monitors, key) == m_monitors.end())
        {
            arrived.push_back(key);
        }
    }
    m_monitors = std::move(keys);
    if (arrived.empty())
    {
        return;
    }
    for (const auto& rule : m_loaded.settings.rules)
    {
        if (rule.trigger != domain::RuleTrigger::MonitorConnected || !rule.monitor)
        {
            continue;
        }
        // edid:<key> waits for that monitor; any other selector for any new monitor.
        const bool matches = rule.monitor->kind != domain::MonitorSelector::Kind::Edid ||
                             std::ranges::find(arrived, rule.monitor->edidKey) != arrived.end();
        if (matches)
        {
            log::info("monitor connected: {}", rule.action.name);
            queueAction(rule.action, Origin::Automatic);
        }
    }
}

void Resident::onPowerSource(bool onAc)
{
    // Windows reports the current source right after we register: only changes count.
    const bool changed = m_onAc.has_value() && *m_onAc != onAc;
    m_onAc = onAc;
    if (!changed)
    {
        return;
    }
    const auto wanted = onAc ? domain::RuleTrigger::OnAc : domain::RuleTrigger::OnBattery;
    for (const auto& rule : m_loaded.settings.rules)
    {
        if (rule.trigger == wanted)
        {
            log::info("{}: {}", onAc ? "plugged in" : "on battery", rule.action.name);
            queueAction(rule.action, Origin::Automatic);
        }
    }
}

void Resident::onClockJump()
{
    // A switch that happened while the computer slept (or the clock moved past it) is applied
    // now; one that happened while we watched is not redone (a manual choice since then stays).
    if (const auto schedule = m_core->schedule())
    {
        const auto gap = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() -
                                                                          m_lastWallClock)
                             .count();
        const std::int32_t now = localSecondsOfDay();
        const bool missed = gap >= domain::kSecondsPerDay ||
                            (gap > 0 && (domain::secondsSince(schedule->lightAt, now) < gap ||
                                         domain::secondsSince(schedule->darkAt, now) < gap));
        if (missed)
        {
            log::info("a scheduled switch passed while away");
            queueScheduleSync();
        }
    }
    armSchedule();
}

bool Resident::due(const std::string& key, domain::TimeOfDay at, std::int32_t now)
{
    const auto since = domain::secondsSince(at, now);
    const auto running = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_started).count();
    if (since > kDueWindowSeconds || since >= running)
    {
        return false; // not just now, or before we started
    }
    const auto fired = m_lastFired.find(key);
    if (fired != m_lastFired.end() && Clock::now() - fired->second < kFiredGuard)
    {
        return false;
    }
    m_lastFired[key] = Clock::now();
    return true;
}

void Resident::onScheduleTimer()
{
    const std::int32_t now = localSecondsOfDay();
    if (const auto schedule = m_core->schedule())
    {
        const bool light = due("light", schedule->lightAt, now);
        const bool dark = due("dark", schedule->darkAt, now);
        if (light || dark)
        {
            log::info("scheduled switch to {}", light ? "light" : "dark");
            queueScheduleSync();
        }
    }
    const auto& rules = m_loaded.settings.rules;
    for (std::size_t i = 0; i < rules.size(); ++i)
    {
        const domain::Rule& rule = rules[i];
        if (rule.trigger != domain::RuleTrigger::Time || !rule.at)
        {
            continue;
        }
        const domain::TimeOfDay at = *rule.at;
        if (due(std::format("rule:{}", i), at, now))
        {
            log::info("rule at {:02}:{:02}: {}", at.hour, at.minute, rule.action.name);
            queueAction(rule.action, Origin::Automatic);
        }
    }
    armSchedule();
}

void Resident::armSchedule()
{
    m_window->killTimer(kScheduleTimer);
    m_lastWallClock = std::chrono::system_clock::now();
    if (safe())
    {
        return;
    }
    const std::int32_t now = localSecondsOfDay();
    std::optional<std::int32_t> next;
    const auto consider = [&](domain::TimeOfDay at) {
        const std::int32_t until = domain::secondsUntil(at, now);
        next = next ? std::min(*next, until) : until;
    };
    if (const auto schedule = m_core->schedule())
    {
        consider(schedule->lightAt);
        consider(schedule->darkAt);
    }
    for (const auto& rule : m_loaded.settings.rules)
    {
        if (rule.trigger == domain::RuleTrigger::Time && rule.at)
        {
            consider(*rule.at);
        }
    }
    if (next)
    {
        const std::chrono::milliseconds wait = std::chrono::seconds(*next) + kScheduleMargin;
        m_window->setTimer(kScheduleTimer, std::min<std::chrono::milliseconds>(wait, kScheduleMaxWait));
    }
}

Json Resident::reloadSettings()
{
    const auto file = settingsFile(m_core->dataDirectory());
    m_settingsStamp = stampOf(file);
    auto loaded = loadSettings(m_core->dataDirectory());
    m_settingsReport = Json{{"present", loaded.present},
                            {"valid", loaded.valid},
                            {"error", loaded.error},
                            {"diagnostics", diagnosticsJson(loaded.diagnostics)}};
    Json result = m_settingsReport;
    if (loaded.valid)
    {
        log::info("settings: {} hotkey(s), {} rule(s){}", loaded.settings.hotkeys.size(),
                  loaded.settings.rules.size(), loaded.settings.keepInTray ? ", kept in the tray" : "");
        applySettings(std::move(loaded));
        return result;
    }
    // Keep what worked before: a typo must not drop every hotkey.
    log::warn("settings.toml not used: {}", loaded.error);
    const auto& problems = loaded.diagnostics.all();
    const std::string first = problems.empty() ? loaded.error
                                               : std::format("line {}: {} ({})", problems.front().line,
                                                             problems.front().message, problems.front().key);
    const UiStrings& text = uiStrings();
    notify(text.settingsErrorTitle, formatted(text.settingsErrorText, utf8To16Lossy(first)));
    publishStatus();
    return result;
}

void Resident::applySettings(LoadedSettings loaded)
{
    m_window->unregisterHotkeys();
    m_emergencyKey = false; // gone with the others; syncBar puts it back
    m_loaded = std::move(loaded);
    setUiLanguage(m_loaded.settings.language);
    const std::string& language = m_loaded.settings.language;
    const bool portuguese = language == domain::kLanguagePortuguese ||
                            (language.empty() && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);
    m_core->setStartOptions(m_loaded.settings.startWinKey,
                            std::string(portuguese ? domain::kLanguagePortuguese : domain::kLanguageEnglish));
    m_core->setTiling(m_loaded.settings.tiling);
    m_lastFired.clear(); // rule numbers may have changed
    m_hotkeyErrors.assign(m_loaded.settings.hotkeys.size(), std::string());
    std::vector<std::string> taken;
    if (!safe())
    {
        const auto& hotkeys = m_loaded.settings.hotkeys;
        for (std::size_t i = 0; i < hotkeys.size(); ++i)
        {
            const auto id = static_cast<std::uint32_t>(i + 1);
            if (auto registered =
                    m_window->registerHotkey(id, hotkeys[i].keys.modifiers, hotkeys[i].keys.virtualKey);
                !registered)
            {
                m_hotkeyErrors[i] = registered.error().message();
                log::warn("hotkey {}: {}", hotkeys[i].keys.toString(), registered.error().toString());
                taken.push_back(hotkeys[i].keys.toString());
            }
        }
    }
    refresh();
    for (const auto& chord : taken)
    {
        notify(uiStrings().actionFailedTitle, formatted(uiStrings().hotkeyTakenText, utf8To16Lossy(chord)));
    }
}

void Resident::refresh()
{
    m_core->syncPluginHost();
    const bool resident = safe() || m_loaded.settings.needsResidency() || m_core->schedule().has_value() ||
                          !m_core->rotation().empty() || m_core->barPlan().has_value() ||
                          m_core->videoPlan().has_value() || m_core->pluginsWanted();
    if (resident != m_resident)
    {
        log::info("{}", resident ? "resident" : "not resident: exits when idle");
    }
    m_resident = resident;
    if (m_resident)
    {
        showTray();
        m_window->killTimer(kIdleTimer);
    }
    else
    {
        m_window->hideTray();
        m_window->setTimer(kIdleTimer, kIdleCheck);
    }
    armSchedule();
    syncPages();
    syncBar();
    syncVideo();
    syncTiler();
    publishStatus();
}

void Resident::syncBar()
{
    const auto plan = m_core->barPlan();
    const bool wanted = !safe() && plan.has_value() && !m_core->barOff();
    // Saving the rice updates its bars: watch its folder while they are shown.
    const auto source = wanted ? m_core->barSource() : std::nullopt;
    if (source != m_watchedSource)
    {
        m_window->unwatchFolder(kRiceWatch);
        m_watchedSource.reset();
        if (source)
        {
            if (auto watched = m_window->watchFolder(kRiceWatch, *source); !watched)
            {
                log::warn("bar: edits to the rice will need an apply: {}", watched.error().toString());
            }
            else
            {
                m_watchedSource = source;
            }
        }
    }
    if (!wanted)
    {
        if (m_barStartedFor)
        {
            m_core->stopBar();
            m_barStartedFor.reset();
        }
        if (m_emergencyKey)
        {
            m_window->unregisterHotkey(kEmergencyHotkey);
            m_emergencyKey = false;
        }
        // Also after a crash of everything: Windows' taskbar is never left hidden.
        platform::showNativeTaskbars();
        return;
    }
    if (!m_emergencyKey)
    {
        if (auto registered = m_window->registerHotkey(kEmergencyHotkey, kEmergencyModifiers, kEmergencyKey);
            !registered)
        {
            log::warn("emergency hotkey Ctrl+Alt+Win+T: {}", registered.error().toString());
        }
        else
        {
            m_emergencyKey = true;
        }
    }
    // A running bar follows the plan file by itself; a new plan restarts one that gave up.
    if (m_barStartedFor && (*m_barStartedFor == *plan || json::boolOr(m_core->barStatus(), "running", false)))
    {
        m_barStartedFor = plan;
        return;
    }
    if (auto started = m_core->startBar(); !started)
    {
        log::error("the bar did not start: {}", started.error().toString());
    }
    m_barStartedFor = plan;
}

void Resident::syncVideo()
{
    const auto plan = m_core->videoPlan();
    const bool wanted = !safe() && plan.has_value();
    if (!wanted)
    {
        if (m_videoStartedFor)
        {
            m_core->stopVideo();
            m_videoStartedFor.reset();
        }
        return;
    }
    // A running rmw-wallpaper follows the plan file by itself; a new plan restarts one that gave up.
    if (m_videoStartedFor &&
        (*m_videoStartedFor == *plan || json::boolOr(m_core->videoStatus(), "running", false)))
    {
        m_videoStartedFor = plan;
        return;
    }
    if (auto started = m_core->startVideo(); !started)
    {
        log::error("the video wallpaper did not start: {}", started.error().toString());
    }
    m_videoStartedFor = plan;
}

void Resident::syncTiler()
{
    const auto plan = m_core->tilerPlan();
    const bool wanted = !safe() && plan.has_value();
    if (!wanted)
    {
        if (m_tilerStartedFor)
        {
            // It puts every window back and leaves by itself; if not, it is ended and the core does it.
            m_tilerStartedFor.reset();
            m_core->stopTiler(false);
            m_window->setTimer(kTilerStopTimer, kTilerStopWait);
        }
        return;
    }
    m_window->killTimer(kTilerStopTimer);
    // A running rmw-tiler follows the plan file by itself; a new plan restarts one that gave up.
    if (m_tilerStartedFor && (*m_tilerStartedFor == *plan || m_core->tilerRunning()))
    {
        m_tilerStartedFor = plan;
        return;
    }
    if (auto started = m_core->startTiler(); !started)
    {
        log::error("the tiler did not start: {}", started.error().toString());
    }
    m_tilerStartedFor = plan;
}

void Resident::emergency()
{
    log::warn("emergency hotkey: the bar is off and Windows' taskbar is back");
    // Right now, on this thread: an operation may be running and the bar may be hung.
    m_core->stopBar();
    m_barStartedFor.reset();
    platform::showNativeTaskbars();
    queueAction({.name = "bar.off", .ricePath = {}, .app = {}}, Origin::User);
    const UiStrings& text = uiStrings();
    notify(text.barOffTitle, std::wstring(text.barOffText));
}

void Resident::onPluginStopped(const std::string& name, const std::string& id)
{
    const UiStrings& text = uiStrings();
    notify(text.pluginStoppedTitle,
           formatted(text.pluginStoppedText, utf8To16Lossy(name), utf8To16Lossy(id)));
    refresh(); // the next plugin host runs without it
}

void Resident::onBarDisabled()
{
    const UiStrings& text = uiStrings();
    notify(text.barOffTitle, std::wstring(text.barDisabledText));
    publishStatus();
}

void Resident::syncPages()
{
    const Json plan = safe() ? Json::object() : m_core->rotation();
    std::erase_if(m_pages, [&](const auto& entry) { return !plan.contains(entry.first); });
    const auto now = Clock::now();
    for (const auto& [field, entry] : plan.items())
    {
        const std::chrono::milliseconds interval{
            std::max<std::int64_t>(json::intOr(entry, "intervalMs", 0), 10'000)};
        auto& pages = m_pages[field];
        if (pages.interval != interval)
        {
            pages = {now + interval, interval}; // new, or another interval: start counting now
        }
    }
    armPages();
}

void Resident::armPages()
{
    m_window->killTimer(kPagesTimer);
    if (m_pages.empty())
    {
        return;
    }
    const auto next =
        std::ranges::min_element(m_pages, {}, [](const auto& entry) { return entry.second.due; });
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(next->second.due - Clock::now());
    m_window->setTimer(kPagesTimer, std::max<std::chrono::milliseconds>(wait, 0ms));
}

void Resident::onPagesTimer()
{
    const auto now = Clock::now();
    for (auto& [field, pages] : m_pages)
    {
        if (pages.due > now + 50ms)
        {
            continue;
        }
        pages.due = now + pages.interval;
        m_work->push([this, name = field] {
            if (auto turned = m_core->turnPage(name); !turned)
            {
                log::warn("slideshow {}: {}", name, turned.error().toString());
            }
        });
    }
    armPages();
}

void Resident::showTray()
{
    const UiStrings& text = uiStrings();
    std::vector<platform::TrayItem> menu;
    // A left click opens the window; the menu is on the right click.
    menu.push_back({.id = kTrayOpen, .label = std::wstring(text.openSettings), .isDefault = true});
    menu.push_back({});
    if (safe())
    {
        menu.push_back({.id = kTraySafeMode, .label = std::wstring(text.safeModeItem), .enabled = false});
        menu.push_back({});
        menu.push_back({.id = kTrayRestore, .label = std::wstring(text.restoreOriginal)});
    }
    else
    {
        menu.push_back({.id = kTrayToggle, .label = std::wstring(text.toggleAppearance)});
        menu.push_back({.id = kTrayRestore, .label = std::wstring(text.restoreOriginal)});
        if (m_core->barPlan())
        {
            menu.push_back(
                {.id = kTrayBar, .label = std::wstring(m_core->barOff() ? text.barOn : text.barOff)});
        }
        menu.push_back({});
        menu.push_back({.id = kTrayReload, .label = std::wstring(text.reloadSettings)});
    }
    menu.push_back({});
    menu.push_back(
        {.id = kTrayAutostart, .label = std::wstring(text.startWithWindows), .checked = startsWithWindows()});
    menu.push_back({.id = kTrayExit, .label = std::wstring(text.exit)});
    if (auto shown = m_window->showTray(safe() ? text.tooltipSafeMode : text.tooltip, std::move(menu));
        !shown)
    {
        log::warn("tray: {}", shown.error().toString());
    }
    else
    {
        m_window->setTimer(kTrayPromoteTimer, 3s); // once Explorer has listed the icon
    }
}

void Resident::queueAction(domain::ActionCall action, Origin origin)
{
    m_work->push([this, action = std::move(action), origin] {
        auto result = m_core->perform(action, origin);
        if (result)
        {
            log::info("{}: done", action.name);
            return;
        }
        log::warn("{} failed: {}", action.name, result.error().toString());
        m_window->post([this, name = action.name, message = result.error().message()] {
            const UiStrings& text = uiStrings();
            notify(text.actionFailedTitle,
                   formatted(text.actionFailedText, utf8To16Lossy(name), utf8To16Lossy(message)));
        });
    });
}

void Resident::queueScheduleSync()
{
    m_work->push([this] {
        if (auto synced = m_core->syncSchedule(); !synced)
        {
            log::warn("scheduled appearance: {}", synced.error().toString());
        }
    });
}

void Resident::notify(std::wstring_view title, const std::wstring& text)
{
    m_window->notify(title, text); // only when the tray icon is there
}

void Resident::publishStatus()
{
    Json hotkeys = Json::array();
    const auto& configured = m_loaded.settings.hotkeys;
    for (std::size_t i = 0; i < configured.size(); ++i)
    {
        const std::string& error = i < m_hotkeyErrors.size() ? m_hotkeyErrors[i] : std::string();
        hotkeys.push_back({{"keys", configured[i].keys.toString()},
                           {"action", configured[i].action.name},
                           {"registered", !safe() && error.empty()},
                           {"error", error}});
    }
    Json status{{"resident", m_resident},
                {"keepInTray", m_loaded.settings.keepInTray},
                {"hotkeys", std::move(hotkeys)},
                {"rules", m_loaded.settings.rules.size()},
                {"settings", m_settingsReport}};
    const std::scoped_lock lock(m_statusMutex);
    m_status = std::move(status);
}

Json Resident::status() const
{
    const std::scoped_lock lock(m_statusMutex);
    return m_status;
}

namespace
{
// 0 Windows' own, 1 English, 2 Portuguese. NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> g_uiLanguage{0};
} // namespace

void setUiLanguage(std::string_view language) noexcept
{
    g_uiLanguage = language == domain::kLanguageEnglish ? 1 : language == domain::kLanguagePortuguese ? 2 : 0;
}

const UiStrings& uiStrings() noexcept
{
    static const bool windowsPortuguese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
    const int chosen = g_uiLanguage.load();
    return chosen == 1 ? kEnglish : chosen == 2 || windowsPortuguese ? kPortuguese : kEnglish;
}

} // namespace rmw::core
