#include "WallpaperApp.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace rmw::wallpaper
{
namespace
{

using namespace std::chrono_literals;

constexpr const wchar_t* kEventsClass = L"RicingManagerWindows.WallpaperEvents";
constexpr const wchar_t* kVideoClass = L"RicingManagerWindows.VideoWallpaper";
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kRebuildTimer = 2;
constexpr UINT_PTR kPlanTimer = 3;
constexpr UINT_PTR kStartTimer = 4;
constexpr UINT kGraphicsReady = WM_APP + 1;
constexpr UINT kTickMs = 1000;
constexpr UINT kStartMs = 100;        // while a renderer starts: is it ready, did it fail
constexpr UINT kRebuildDelayMs = 500; // Explorer, monitors and DPI settle first
constexpr UINT kPlanDelayMs = 200;    // the core writes the plan in one go, but let it finish
constexpr DWORD kLoopMs = 250;        // the heartbeat beats at least this often
constexpr auto kGraphicsRetry = 30s;
constexpr auto kBusyReport = 1s; // the core ends the worker after 1.5 s without a heartbeat
constexpr std::uint64_t kPlanMaxBytes = 1u << 20;

WallpaperApp* g_app = nullptr; // the window procedure's

bool isShellClass(std::wstring_view name)
{
    constexpr std::array shell = {
        std::wstring_view(L"Progman"), std::wstring_view(L"WorkerW"), std::wstring_view(L"Shell_TrayWnd"),
        std::wstring_view(L"Shell_SecondaryTrayWnd"), std::wstring_view(L"RicingManagerWindows.Bar")};
    return std::ranges::find(shell, name) != shell.end();
}

std::wstring className(HWND window)
{
    std::array<wchar_t, 128> name{};
    const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    return {name.data(), static_cast<std::size_t>(std::max(length, 0))};
}

bool ours(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

bool cloaked(HWND window)
{
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

std::filesystem::path statusFile(const std::filesystem::path& planFile)
{
    return planFile.parent_path() / L"status.json";
}

bool covers(const RECT& outer, const RECT& inner)
{
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
           outer.bottom >= inner.bottom;
}

LRESULT CALLBACK videoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // the desktop below gets every click
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// While it lives, the window thread is "busy with" its name (for the watch thread's log).
class Busy
{
public:
    Busy(std::atomic<const char*>& with, std::atomic<video::Clock::rep>& since, const char* what) noexcept
        : m_with(&with)
        , m_since(&since)
        , m_previous(with.exchange(what))
    {
        since = video::Clock::now().time_since_epoch().count();
    }
    ~Busy()
    {
        *m_with = m_previous;
        *m_since = video::Clock::now().time_since_epoch().count();
    }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
    Busy(Busy&&) = delete;
    Busy& operator=(Busy&&) = delete;

private:
    std::atomic<const char*>* m_with;
    std::atomic<video::Clock::rep>* m_since;
    const char* m_previous;
};

} // namespace

struct WallpaperApp::Access
{
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_app != nullptr && window == g_app->m_events)
        {
            g_app->onMessage(message, wParam, lParam);
            if (message == WM_POWERBROADCAST)
            {
                return TRUE;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

struct WallpaperApp::GraphicsJob
{
    std::mutex mutex;
    std::optional<Result<std::shared_ptr<Graphics>>> result;
};

WallpaperApp::WallpaperApp(std::filesystem::path planFile, platform::Heartbeat heartbeat)
    : m_planFile(std::move(planFile))
    , m_heartbeat(std::move(heartbeat))
    , m_taskbarCreated(RegisterWindowMessageW(L"TaskbarCreated"))
{
    g_app = this;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW events{};
    events.cbSize = sizeof(events);
    events.lpfnWndProc = &Access::proc;
    events.hInstance = instance;
    events.lpszClassName = kEventsClass;
    RegisterClassExW(&events);
    WNDCLASSEXW video{};
    video.cbSize = sizeof(video);
    video.lpfnWndProc = &videoProc;
    video.hInstance = instance;
    video.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video.lpszClassName = kVideoClass;
    RegisterClassExW(&video);
    // Top-level (hidden): a message-only window misses TaskbarCreated and WM_DISPLAYCHANGE.
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kEventsClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  instance, nullptr);
    m_events = window;
    if (window != nullptr)
    {
        ChangeWindowMessageFilterEx(window, m_taskbarCreated, MSGFLT_ALLOW, nullptr);
        WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
        m_displayNotification = RegisterPowerSettingNotification(window, &GUID_CONSOLE_DISPLAY_STATE,
                                                                 DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(window, kTickTimer, kTickMs, nullptr);
    }
    m_watch = std::jthread([this](const std::stop_token& stop) { watchWindowThread(stop); });
}

WallpaperApp::~WallpaperApp()
{
    clear();
    if (m_displayNotification != nullptr)
    {
        UnregisterPowerSettingNotification(static_cast<HPOWERNOTIFY>(m_displayNotification));
    }
    if (m_events != nullptr)
    {
        WTSUnRegisterSessionNotification(static_cast<HWND>(m_events));
        DestroyWindow(static_cast<HWND>(m_events));
    }
    g_app = nullptr;
}

void WallpaperApp::watchWindowThread(const std::stop_token& stop)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    video::Clock::rep reported = 0;
    while (!stop.stop_requested())
    {
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, 250ms, [] { return false; });
        }
        const char* with = m_busyWith.load();
        const auto since = m_busySince.load();
        const auto busyFor = std::chrono::duration_cast<std::chrono::milliseconds>(
            video::Clock::now() - video::Clock::time_point(video::Clock::duration(since)));
        if (with != nullptr && busyFor >= kBusyReport && since != reported)
        {
            reported = since;
            log::warn("wallpaper: the window thread has been busy for {} ms: {}", busyFor.count(), with);
        }
    }
}

bool WallpaperApp::loadPlan()
{
    auto bytes = platform::readFileLimited(m_planFile, kPlanMaxBytes);
    if (!bytes)
    {
        log::info("wallpaper: no plan anymore");
        return false;
    }
    auto plan =
        video::planFromJson(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!plan)
    {
        log::error("wallpaper: {}", plan.error().toString());
        return !m_plan.entries.empty(); // keep playing what we had
    }
    if (!plan->hasVideo())
    {
        log::info("wallpaper: the plan has no video anymore");
        return false;
    }
    if (*plan != m_plan)
    {
        m_plan = std::move(*plan);
        m_rebuildPending = true;
    }
    return true;
}

int WallpaperApp::run()
{
    if (m_events == nullptr)
    {
        log::error("wallpaper: no event window");
        return 1;
    }
    if (!loadPlan())
    {
        return 0;
    }
    rebuild();
    HANDLE watch = FindFirstChangeNotificationW(m_planFile.parent_path().c_str(), FALSE,
                                                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    while (!m_quit)
    {
        const DWORD count = watch != INVALID_HANDLE_VALUE ? 1 : 0;
        const DWORD signaled =
            MsgWaitForMultipleObjectsEx(count, &watch, kLoopMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        m_heartbeat.beat();
        if (count == 1 && signaled == WAIT_OBJECT_0)
        {
            FindNextChangeNotification(watch);
            SetTimer(static_cast<HWND>(m_events), kPlanTimer, kPlanDelayMs, nullptr);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                m_quit = true;
                break;
            }
            DispatchMessageW(&message);
        }
    }
    if (watch != INVALID_HANDLE_VALUE)
    {
        FindCloseChangeNotification(watch);
    }
    clear();
    std::error_code ec;
    std::filesystem::remove(statusFile(m_planFile), ec); // nothing plays anymore
    return m_exitCode;
}

void WallpaperApp::onMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
    auto* const events = static_cast<HWND>(m_events);
    if (message == m_taskbarCreated && m_taskbarCreated != 0)
    {
        log::info("wallpaper: Explorer restarted; going behind the icons again");
        SetTimer(events, kRebuildTimer, kRebuildDelayMs, nullptr);
        return;
    }
    switch (message)
    {
    case WM_TIMER:
        if (wParam == kTickTimer)
        {
            tick();
        }
        else if (wParam == kStartTimer)
        {
            if (!watchRenderers())
            {
                KillTimer(events, kStartTimer);
            }
            writeStatus();
        }
        else if (wParam == kRebuildTimer)
        {
            KillTimer(events, kRebuildTimer);
            rebuild();
        }
        else if (wParam == kPlanTimer)
        {
            KillTimer(events, kPlanTimer);
            if (!loadPlan())
            {
                m_quit = true;
            }
            else if (m_rebuildPending)
            {
                rebuild();
            }
        }
        break;
    case kGraphicsReady:
        onGraphics();
        break;
    case WM_DISPLAYCHANGE:
        SetTimer(events, kRebuildTimer, kRebuildDelayMs, nullptr);
        break;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK)
        {
            m_session.locked = wParam == WTS_SESSION_LOCK;
            updatePauses();
        }
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND)
        {
            m_session.suspending = true;
            updatePauses();
        }
        else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
        {
            m_session.suspending = false;
            // The GPU may have been reset while asleep: start over.
            m_graphics.reset();
            SetTimer(events, kRebuildTimer, kRebuildDelayMs, nullptr);
        }
        else if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0)
        {
            const auto* setting =
                reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam); // NOLINT(performance-no-int-to-ptr)
            if (setting->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && setting->DataLength >= sizeof(DWORD))
            {
                DWORD state = 0;
                std::memcpy(&state, setting->Data, sizeof(state));
                m_session.displayOff = state == 0; // 0 off, 1 on, 2 dimmed
                updatePauses();
            }
        }
        else if (wParam == PBT_APMPOWERSTATUSCHANGE)
        {
            updatePauses();
        }
        break;
    default:
        break;
    }
}

void WallpaperApp::clear()
{
    const Busy busy(m_busyWith, m_busySince, "stopping the renderers");
    for (auto& screen : m_screens)
    {
        screen.renderer.reset(); // its thread first
        if (screen.window != nullptr)
        {
            DestroyWindow(static_cast<HWND>(screen.window));
        }
    }
    m_screens.clear();
}

void WallpaperApp::startGraphics()
{
    if (m_graphicsJob)
    {
        return; // on its way
    }
    auto job = std::make_shared<GraphicsJob>();
    m_graphicsJob = job;
    auto* const events = static_cast<HWND>(m_events);
    // Detached: a GPU that keeps it waiting must not keep this process from ending.
    std::thread([job, events] {
        auto graphics = Graphics::create();
        {
            const std::scoped_lock lock(job->mutex);
            job->result = std::move(graphics);
        }
        PostMessageW(events, kGraphicsReady, 0, 0);
    }).detach();
}

void WallpaperApp::onGraphics()
{
    if (!m_graphicsJob)
    {
        return;
    }
    std::optional<Result<std::shared_ptr<Graphics>>> result;
    {
        const std::scoped_lock lock(m_graphicsJob->mutex);
        result = std::move(m_graphicsJob->result);
    }
    if (!result)
    {
        return;
    }
    m_graphicsJob.reset();
    if (!*result)
    {
        log::error("wallpaper: no GPU to play on ({}); the rice's fallback image stays",
                   result->error().toString());
        m_error = std::format("no GPU to play on: {}", result->error().toString());
        m_graphicsFailedAt = video::Clock::now();
        writeStatus();
        return;
    }
    m_graphics = std::move(**result);
    m_graphicsFailedAt.reset();
    log::info("wallpaper: playing on {}", m_graphics->adapter());
    rebuild();
}

void WallpaperApp::rebuild()
{
    const Busy busy(m_busyWith, m_busySince, "rebuilding");
    m_rebuildPending = false;
    clear();
    m_error.clear();
    if (!m_graphics)
    {
        startGraphics(); // here again once the device is made
        writeStatus();
        return;
    }
    platform::Win32DisplayTopology topology;
    auto monitors = topology.monitors();
    if (!monitors)
    {
        log::error("wallpaper: monitors: {}", monitors.error().toString());
        m_error = std::format("monitors: {}", monitors.error().toString());
        writeStatus();
        return;
    }
    std::vector<domain::MonitorFacts> facts;
    facts.reserve(monitors->size());
    for (const auto& monitor : *monitors)
    {
        facts.push_back(
            {monitor.identityKey, monitor.primary, monitor.left, monitor.top, monitor.right, monitor.bottom});
    }
    const auto chosen = video::videoFor(m_plan, facts);
    for (std::size_t i = 0; i < monitors->size(); ++i)
    {
        if (chosen[i] < 0)
        {
            continue;
        }
        const auto& monitor = (*monitors)[i];
        Screen screen;
        screen.monitor = monitor.identityKey;
        screen.left = monitor.left;
        screen.top = monitor.top;
        screen.width = monitor.width();
        screen.height = monitor.height();
        screen.entry = static_cast<std::size_t>(chosen[i]);
        if (!buildScreen(screen) && screen.window != nullptr)
        {
            // Kept, with its error, for the status; tried again at the next rebuild.
            DestroyWindow(static_cast<HWND>(screen.window));
            screen.window = nullptr;
        }
        m_screens.push_back(std::move(screen));
    }
    log::info("wallpaper: a video for {} monitor(s)", m_screens.size());
    updatePauses();
    writeStatus();
}

bool WallpaperApp::buildScreen(Screen& screen)
{
    if (screen.window == nullptr)
    {
        // Hidden until its first frame: until then Windows' wallpaper (the fallback image) shows.
        screen.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                                        kVideoClass, L"", WS_POPUP, screen.left, screen.top, screen.width,
                                        screen.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (screen.window == nullptr)
        {
            screen.error = std::format("cannot create a window ({})", GetLastError());
            log::error("wallpaper: {}", screen.error);
            return false;
        }
        const Busy busy(m_busyWith, m_busySince, "going behind the desktop icons");
        auto attached =
            attachBehindIcons(screen.window, screen.left, screen.top, screen.width, screen.height);
        if (!attached)
        {
            screen.error = attached.error().toString();
            log::error("wallpaper: {}", screen.error);
            return false;
        }
        screen.attachment = *attached;
    }
    const auto& entry = m_plan.entries[screen.entry];
    screen.renderer = std::make_unique<Renderer>(
        m_graphics, screen.window,
        RendererOptions{entry.file, entry.fit, m_plan.policy.fpsLimit, screen.width, screen.height});
    screen.renderer->start();
    screen.error.clear();
    screen.video.clear();
    screen.announced = false;
    screen.shown = false;
    SetTimer(static_cast<HWND>(m_events), kStartTimer, kStartMs, nullptr);
    return true;
}

void WallpaperApp::release(Screen& screen)
{
    // The decoder and the swap chain go; Windows' own wallpaper (the rice's fallback image) shows
    // where the window was.
    const Busy busy(m_busyWith, m_busySince, "releasing a monitor's video");
    screen.renderer.reset();
    if (screen.window != nullptr)
    {
        DestroyWindow(static_cast<HWND>(screen.window)); // a fresh one when it plays again
        screen.window = nullptr;
    }
    screen.shown = false;
}

bool WallpaperApp::watchRenderers()
{
    bool starting = false;
    for (auto& screen : m_screens)
    {
        if (!screen.renderer)
        {
            continue;
        }
        const std::string file = utf16To8Lossy(m_plan.entries[screen.entry].file.filename().native());
        if (screen.renderer->failed())
        {
            if (screen.renderer->deviceLost())
            {
                log::warn("wallpaper: the GPU went away; starting over");
                m_graphics.reset();
                rebuild();
                return false;
            }
            if (screen.renderer->ready())
            {
                screen.error = std::format("the video stopped: {}", screen.renderer->failure());
                log::warn("wallpaper: {}: {}; Windows' wallpaper shows", screen.monitor, screen.error);
            }
            else
            {
                screen.error = screen.renderer->failure();
                log::error("wallpaper: {}: {}", file, screen.error);
            }
            release(screen);
            continue;
        }
        if (!screen.announced)
        {
            if (!screen.renderer->ready())
            {
                starting = true;
                continue;
            }
            screen.announced = true;
            screen.video = screen.renderer->describe();
            log::info("wallpaper: {} on {} ({})", file, screen.monitor, screen.video);
        }
        if (!screen.shown)
        {
            if (screen.renderer->framesPresented() == 0)
            {
                // A paused one shows nothing yet; it is shown when it plays (the tick looks).
                starting = starting || screen.paused == video::PauseReason::None;
                continue;
            }
            ShowWindow(static_cast<HWND>(screen.window), SW_SHOWNA);
            screen.shown = true;
        }
    }
    return starting;
}

void WallpaperApp::updatePauses()
{
    {
        const Busy busy(m_busyWith, m_busySince, "asking for the power status");
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) != FALSE)
        {
            m_session.onBattery = power.ACLineStatus == 0;
            m_session.batterySaver = power.SystemStatusFlag == 1;
        }
    }
    // What is in front, and what is maximized, on each monitor.
    HWND foreground = GetForegroundWindow();
    RECT foregroundRect{};
    const bool foregroundCounts = foreground != nullptr && !ours(foreground) &&
                                  !isShellClass(className(foreground)) &&
                                  GetWindowRect(foreground, &foregroundRect) != FALSE;
    bool d3dFullscreen = false;
    {
        const Busy busy(m_busyWith, m_busySince, "asking the shell for a fullscreen app");
        QUERY_USER_NOTIFICATION_STATE notification{};
        d3dFullscreen =
            SUCCEEDED(SHQueryUserNotificationState(&notification)) &&
            (notification == QUNS_RUNNING_D3D_FULL_SCREEN || notification == QUNS_PRESENTATION_MODE);
    }
    std::vector<RECT> maximized;
    {
        const Busy busy(m_busyWith, m_busySince, "looking for maximized windows");
        EnumWindows(
            [](HWND window, LPARAM param) -> BOOL {
                if (IsWindowVisible(window) != FALSE && IsIconic(window) == FALSE &&
                    IsZoomed(window) != FALSE && !cloaked(window) && !ours(window))
                {
                    RECT rect{};
                    if (GetWindowRect(window, &rect) != FALSE)
                    {
                        // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands our pointer back
                        auto* const list = reinterpret_cast<std::vector<RECT>*>(param);
                        list->push_back(rect);
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&maximized));
    }

    const auto now = video::Clock::now();
    for (auto& screen : m_screens)
    {
        const RECT area{screen.left, screen.top, screen.left + screen.width, screen.top + screen.height};
        video::MonitorState state;
        if (foregroundCounts)
        {
            const bool onThis = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) ==
                                MonitorFromPoint({screen.left + 1, screen.top + 1}, MONITOR_DEFAULTTONULL);
            state.fullscreen = onThis && (covers(foregroundRect, area) || d3dFullscreen);
        }
        state.covered = state.fullscreen || std::ranges::any_of(maximized, [&](const RECT& rect) {
                            const POINT middle{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
                            return middle.x >= area.left && middle.x < area.right && middle.y >= area.top &&
                                   middle.y < area.bottom;
                        });
        const auto reason = video::pauseReason(m_plan.policy, m_session, state);
        if (reason != screen.paused)
        {
            log::info("wallpaper: {}: {}", screen.monitor,
                      reason == video::PauseReason::None
                          ? std::string("playing")
                          : std::format("paused: {}", video::toString(reason)));
            screen.paused = reason;
            screen.pausedSince = reason == video::PauseReason::None ? std::nullopt : std::optional(now);
            if (reason == video::PauseReason::None && !screen.renderer && screen.error.empty())
            {
                if (!buildScreen(screen))
                {
                    continue;
                }
            }
            if (screen.renderer)
            {
                screen.renderer->setPaused(reason != video::PauseReason::None);
            }
        }
        if (screen.renderer && screen.pausedSince && now - *screen.pausedSince >= video::kReleaseAfter)
        {
            release(screen);
            log::info("wallpaper: {}: paused for {} s, memory released", screen.monitor,
                      video::kReleaseAfter.count());
        }
    }
}

void WallpaperApp::tick()
{
    // A test hook for the system tests (docs/04 M6: "a hung window thread is found in < 2 s").
    const auto hang = m_planFile.parent_path() / L"test-hang";
    std::error_code ec;
    if (std::filesystem::exists(hang, ec))
    {
        std::filesystem::remove(hang, ec);
        log::warn("wallpaper: TEST HOOK: the window thread hangs for 10 s");
        const Busy busy(m_busyWith, m_busySince, "the test hook");
        std::this_thread::sleep_for(10s);
    }
    if (!m_graphics)
    {
        if (!m_graphicsJob && m_graphicsFailedAt &&
            video::Clock::now() - *m_graphicsFailedAt >= kGraphicsRetry)
        {
            rebuild();
        }
        return;
    }
    [[maybe_unused]] const bool starting = watchRenderers();
    if (!m_graphics)
    {
        return; // the GPU went away: rebuilding
    }
    for (auto& screen : m_screens)
    {
        if (!screen.shown)
        {
            continue;
        }
        const Busy busy(m_busyWith, m_busySince, "checking that it is behind the icons");
        if (!stillAttached(screen.window, screen.attachment))
        {
            log::info("wallpaper: {}: the desktop changed; going behind the icons again", screen.monitor);
            if (auto attached =
                    attachBehindIcons(screen.window, screen.left, screen.top, screen.width, screen.height))
            {
                screen.attachment = *attached;
            }
            else
            {
                SetTimer(static_cast<HWND>(m_events), kRebuildTimer, kRebuildDelayMs, nullptr);
            }
        }
    }
    updatePauses();
    writeStatus();
}

void WallpaperApp::writeStatus()
{
    json::Json monitors = json::Json::array();
    for (const auto& screen : m_screens)
    {
        const char* state = !screen.error.empty()                       ? "failed"
                            : screen.renderer == nullptr                ? "released"
                            : !screen.announced                         ? "starting"
                            : screen.paused != video::PauseReason::None ? "paused"
                                                                        : "playing";
        json::Json monitor{{"monitor", screen.monitor}, {"state", state}, {"video", screen.video}};
        if (screen.paused != video::PauseReason::None)
        {
            monitor["paused"] = std::string(video::toString(screen.paused));
        }
        if (!screen.error.empty())
        {
            monitor["error"] = screen.error;
        }
        monitors.push_back(std::move(monitor));
    }
    json::Json status{{"version", 1}, {"monitors", std::move(monitors)}};
    if (m_graphics)
    {
        status["adapter"] = m_graphics->adapter();
    }
    if (!m_error.empty())
    {
        status["error"] = m_error;
    }
    std::string text = json::dump(status);
    if (text == m_lastStatus)
    {
        return;
    }
    const Busy busy(m_busyWith, m_busySince, "writing status.json");
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    if (auto written = platform::writeFileAtomic(statusFile(m_planFile), {bytes, text.size()}); !written)
    {
        log::warn("wallpaper: status: {}", written.error().toString());
        return;
    }
    m_lastStatus = std::move(text);
}

} // namespace rmw::wallpaper
