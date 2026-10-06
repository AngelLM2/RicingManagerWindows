#include "TilerApp.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <utility>

namespace rmw::tilerapp
{
namespace
{

using domain::TilingCommand;

constexpr UINT_PTR kLayoutTimer = 1;
constexpr UINT_PTR kSettleTimer = 2;
constexpr UINT_PTR kSaveTimer = 3;
constexpr UINT_PTR kPlanTimer = 4;
constexpr UINT_PTR kMonitorTimer = 5;
constexpr UINT_PTR kFocusTimer = 6;
constexpr UINT_PTR kAdoptTimer = 7;
constexpr UINT kAnimationDone = WM_APP + 31; // the Animator: every move is over
// A window that shows is tiled when it is still there a moment later: one shown for an instant (a splash,
// a system popup) moved every window twice (v1.7, seen in the Sandbox as a jolt in workspace switches: it
// lasted 18 ms). Short, so a window that opens is not seen for long where its app put it.
constexpr UINT kAdoptDelayMs = 60;
constexpr UINT kAdoptTickMs = 20;
constexpr std::uint64_t kSwitchCalmMs = 400; // after our own switch, a window coming to the front is its echo
constexpr UINT kSettleDelayMs = 200;
constexpr UINT kAfterAnimationMs = 60;
constexpr std::int32_t kSlideMin = 8; // closer than this to its place: a window just goes there
constexpr auto kSwitchDuration = std::chrono::milliseconds(220);
constexpr auto kMoveDuration = std::chrono::milliseconds(180);
constexpr int kSettleRounds = 2;
constexpr UINT kFocusDelayMs = 30; // after the shows: a window still hidden takes no focus
constexpr DWORD kBeatMs = 250;
constexpr std::uint64_t kAreaCheckMs = 2000;
constexpr std::uint64_t kMaxPlanBytes = std::uint64_t{256} << 10;

TilerApp* g_app = nullptr; // the one app of this process (the WinEvent callback has no context)

std::optional<tiling::Direction> directionOf(TilingCommand command) noexcept
{
    switch (command)
    {
    case TilingCommand::FocusLeft:
    case TilingCommand::MoveLeft:
        return tiling::Direction::Left;
    case TilingCommand::FocusRight:
    case TilingCommand::MoveRight:
        return tiling::Direction::Right;
    case TilingCommand::FocusUp:
    case TilingCommand::MoveUp:
        return tiling::Direction::Up;
    case TilingCommand::FocusDown:
    case TilingCommand::MoveDown:
        return tiling::Direction::Down;
    default:
        return std::nullopt;
    }
}

bool nearly(const tiling::Rect& a, const tiling::Rect& b) noexcept
{
    return std::abs(a.left - b.left) <= 1 && std::abs(a.top - b.top) <= 1 && std::abs(a.right - b.right) <= 1 &&
           std::abs(a.bottom - b.bottom) <= 1;
}

std::uint32_t colorref(const domain::Color& color) noexcept
{
    return static_cast<std::uint32_t>(RGB(color.r, color.g, color.b));
}

struct FollowerSearch
{
    HWND owner = nullptr;
    std::vector<tiling::WindowId> found;
};

BOOL CALLBACK collectFollower(HWND window, LPARAM context)
{
    auto& search = *reinterpret_cast<FollowerSearch*>(context); // NOLINT(performance-no-int-to-ptr)
    // Its dialogs (they have a title bar); never a tooltip, a menu or a list that drops down, which
    // came back as ghosts when shown again later.
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    if (GetWindow(window, GW_OWNER) == search.owner && IsWindowVisible(window) != FALSE &&
        (style & WS_CAPTION) == WS_CAPTION)
    {
        search.found.push_back(idOf(window));
    }
    return TRUE;
}

BOOL CALLBACK collectWindow(HWND window, LPARAM context)
{
    reinterpret_cast<std::vector<HWND>*>(context)->push_back(window); // NOLINT(performance-no-int-to-ptr)
    return TRUE;
}

} // namespace

struct TilerApp::Access
{
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_app != nullptr && g_app->m_window == window)
        {
            return g_app->handle(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void CALLBACK onEvent(HWINEVENTHOOK /*hook*/, DWORD event, HWND window, LONG object, LONG child,
                                 DWORD /*thread*/, DWORD /*time*/)
    {
        if (g_app != nullptr && window != nullptr && object == OBJID_WINDOW && child == CHILDID_SELF)
        {
            g_app->onEvent(event, window);
        }
    }
};

TilerApp::TilerApp(std::filesystem::path planFile, platform::Heartbeat& heartbeat, void* core, Options options)
    : m_planFile(std::move(planFile))
    , m_windowsFile(m_planFile.parent_path() / L"windows.json")
    , m_heartbeat(&heartbeat)
    , m_core(core)
    , m_options(options)
    , m_window(makeControlWindow())
    , m_shared(options.elevated)
    , m_animator(std::make_unique<Animator>(m_window, kAnimationDone))
{
    g_app = this;
    // A tiler that ended badly left its windows hidden or out of place: back first. Those of an admin app
    // wait (in the file) for a tiler that may move them.
    m_waiting = putBackFromFile(m_windowsFile);
    if (m_options.elevated && m_window != nullptr)
    {
        // The bars (not elevated) ask for a workspace; the core asks it to leave.
        ChangeWindowMessageFilterEx(static_cast<HWND>(m_window), tiling::kGoToMessage, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(static_cast<HWND>(m_window), WM_CLOSE, MSGFLT_ALLOW, nullptr);
    }

    const std::array<std::pair<DWORD, DWORD>, 4> ranges = {{
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    }};
    for (const auto& [first, last] : ranges)
    {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, &Access::onEvent, 0, 0,
                                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
        {
            m_hooks.push_back(hook);
        }
    }
    m_planWatch = FindFirstChangeNotificationW(m_planFile.parent_path().c_str(), FALSE,
                                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (m_planWatch == INVALID_HANDLE_VALUE)
    {
        m_planWatch = nullptr;
    }
}

TilerApp::~TilerApp()
{
    try
    {
        stop();
    }
    catch (...) // NOLINT(bugprone-empty-catch): the hooks and the window must still go below
    {
    }
    m_animator.reset();
    m_keys.reset();
    for (void* hook : m_hooks)
    {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook));
    }
    if (m_planWatch != nullptr)
    {
        FindCloseChangeNotification(m_planWatch);
    }
    m_virtual.reset();
    if (m_window != nullptr)
    {
        DestroyWindow(static_cast<HWND>(m_window));
    }
    g_app = nullptr;
}

void* TilerApp::makeControlWindow()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Access::windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = tiling::kControlClass;
    RegisterClassExW(&windowClass);
    // Top-level (not message-only): it hears the session end and the monitors change.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, tiling::kControlClass, L"Ricing Manager tiler",
                           WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

TilerApp::Ending TilerApp::run()
{
    if (m_window == nullptr)
    {
        log::error("tiler: no control window ({})", GetLastError());
        return Ending::Failed;
    }
    if (!readPlan())
    {
        std::error_code ec;
        if (!std::filesystem::exists(m_planFile, ec))
        {
            log::info("tiler: no plan; nothing to do");
            return Ending::Done;
        }
        // There, but not readable yet (being written): read again on its next change, or in a moment.
        log::warn("tiler: the plan cannot be read yet; trying again");
        SetTimer(static_cast<HWND>(m_window), kPlanTimer, 500, nullptr);
    }
    log::info("tiler: running{}", m_options.elevated ? " as administrator" : "");
    while (!m_quit)
    {
        std::array<HANDLE, 3> handles{};
        DWORD count = 0;
        if (m_core != nullptr)
        {
            handles[count++] = m_core;
        }
        if (m_planWatch != nullptr)
        {
            handles[count++] = m_planWatch;
        }
        if (m_virtual && m_virtual->changed() != nullptr)
        {
            handles[count++] = m_virtual->changed();
        }
        const DWORD woke = MsgWaitForMultipleObjectsEx(count, handles.data(), kBeatMs, QS_ALLINPUT,
                                                       MWMO_INPUTAVAILABLE);
        m_heartbeat->beat();
        // A bar that ends without a word gives its strip back silently: the work areas are
        // compared now and then (a few microseconds).
        if (const auto now = GetTickCount64(); now - m_lastAreaCheck >= kAreaCheckMs)
        {
            m_lastAreaCheck = now;
            if (m_desktop && monitorsNow() != m_monitors)
            {
                syncMonitors();
            }
        }
        if (woke < WAIT_OBJECT_0 + count) // WAIT_OBJECT_0 is 0
        {
            HANDLE signaled = handles[woke - WAIT_OBJECT_0];
            if (signaled == m_core)
            {
                log::info("tiler: the core is gone; every window goes back");
                break;
            }
            if (signaled == m_planWatch)
            {
                FindNextChangeNotification(m_planWatch);
                SetTimer(static_cast<HWND>(m_window), kPlanTimer, 100, nullptr);
            }
            else if (m_virtual && signaled == m_virtual->changed())
            {
                m_virtual->rearm();
                syncDesktops();
            }
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                m_quit = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    stop();
    log::info("tiler: stopped");
    return m_ending;
}

std::intptr_t TilerApp::handle(std::uint32_t message, std::uintptr_t wParam, std::intptr_t lParam)
{
    HWND self = static_cast<HWND>(m_window);
    switch (message)
    {
    case WM_TIMER:
        switch (wParam)
        {
        case kLayoutTimer:
            KillTimer(self, kLayoutTimer);
            layout();
            break;
        case kSettleTimer:
            settle();
            break;
        case kSaveTimer:
            KillTimer(self, kSaveTimer);
            save();
            break;
        case kPlanTimer:
            KillTimer(self, kPlanTimer);
            if (!readPlan())
            {
                std::error_code ec;
                if (std::filesystem::exists(m_planFile, ec))
                {
                    SetTimer(self, kPlanTimer, 500, nullptr); // there but unreadable for now
                    break;
                }
                log::info("tiler: the plan is gone; every window goes back");
                m_quit = true;
            }
            break;
        case kMonitorTimer:
            KillTimer(self, kMonitorTimer);
            syncMonitors();
            break;
        case kAdoptTimer: {
            // Each window when its own wait is over: popups showing all the time (a player's controls,
            // tooltips) never put off a window that opened.
            const std::uint64_t now = GetTickCount64();
            std::vector<tiling::WindowId> due;
            for (auto it = m_appearing.begin(); it != m_appearing.end();)
            {
                if (now - it->second >= kAdoptDelayMs)
                {
                    due.push_back(it->first);
                    it = m_appearing.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (m_appearing.empty())
            {
                KillTimer(self, kAdoptTimer);
                m_adoptTimer = false;
            }
            for (const tiling::WindowId id : due)
            {
                if (IsWindow(static_cast<HWND>(windowOf(id))) != FALSE)
                {
                    adopt(windowOf(id)); // checks again that it is an app window, shown
                }
            }
            break;
        }
        case kFocusTimer:
            KillTimer(self, kFocusTimer);
            // Like a Mac: never a window of another desktop (Windows would go back to that desktop to
            // show it — a switch right after another one bounced back).
            if (m_pendingFocus && IsWindow(static_cast<HWND>(windowOf(*m_pendingFocus))) != FALSE &&
                onShownDesktop(windowOf(*m_pendingFocus)))
            {
                focus(windowOf(*m_pendingFocus));
            }
            m_pendingFocus.reset();
            break;
        default:
            break;
        }
        return 0;
    case kAnimationDone:
        // Whatever an app did meanwhile (a size of its own, a late answer) is looked at now.
        m_settleRounds = kSettleRounds;
        SetTimer(self, kSettleTimer, kAfterAnimationMs, nullptr);
        return 0;
    case KeyHook::kCommandMessage:
        if (wParam < domain::kTilingCommandCount)
        {
            command(static_cast<TilingCommand>(wParam), static_cast<int>(lParam));
        }
        return 0;
    case tiling::kGoToMessage: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int workspace = static_cast<int>(wParam) - 1;
        if (mac() && m_virtual)
        {
            VirtualDesktops::goTo(workspace, m_virtual->read()); // as Windows has it now (see Workspace)
            return 0;
        }
        for (std::size_t i = 0; i < m_monitors.size(); ++i)
        {
            if (m_monitors[i].bounds.contains(x, y))
            {
                goTo(i, workspace);
                break;
            }
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        SetTimer(self, kMonitorTimer, 200, nullptr);
        return 0;
    case WM_SETTINGCHANGE:
        // A bar took or gave back its strip (Explorer does not always say SPI_SETWORKAREA).
        SetTimer(self, kMonitorTimer, 100, nullptr);
        return 0;
    case WM_QUERYENDSESSION:
        // Apps asking "save your work?" must be seen: every window back now.
        stop();
        return TRUE;
    case WM_ENDSESSION:
        if (wParam != FALSE)
        {
            m_quit = true;
        }
        else if (!m_paused && m_plan && m_plan->settings.enabled)
        {
            start(); // the shutdown was cancelled: tiling again
        }
        return 0;
    case WM_CLOSE:
        m_quit = true;
        return 0;
    default:
        return DefWindowProcW(self, message, wParam, lParam);
    }
}

bool TilerApp::readPlan()
{
    std::error_code ec;
    if (!std::filesystem::exists(m_planFile, ec))
    {
        return false;
    }
    auto bytes = platform::readFileLimited(m_planFile, kMaxPlanBytes);
    if (!bytes)
    {
        return m_plan.has_value(); // being rewritten, maybe: the next change reads it again
    }
    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (text == m_planText)
    {
        return true;
    }
    auto plan = tiling::parsePlan(text);
    if (!plan)
    {
        log::warn("tiler: {}", plan.error().toString());
        return m_plan.has_value();
    }
    m_planText = std::move(text);
    applyPlan(std::move(*plan));
    return true;
}

void TilerApp::applyPlan(tiling::Plan plan)
{
    const std::optional<tiling::Plan> old = std::move(m_plan);
    const domain::TilingSettings& now = m_plan.emplace(std::move(plan)).settings;
    // Advanced mode (admin apps' windows too): an elevated copy does the work, or stops doing it. Before
    // the look: painting every window to reset it a moment later made them flicker.
    if (now.enabled && now.runAsAdmin && !m_options.elevated && !m_options.adminDeclined)
    {
        m_ending = Ending::BeAdmin;
        m_quit = true;
        return;
    }
    if ((!now.enabled || !now.runAsAdmin) && m_options.elevatedCopy)
    {
        m_ending = Ending::BePlain;
        m_quit = true;
        return;
    }
    m_looks.set(m_plan->look);
    if (now.enabled && (!old || !old->settings.enabled))
    {
        m_paused = false; // turned on in the settings: a Toggle from before does not keep it off
    }
    // The keyboard is watched only while tiling: the window look alone hears no key.
    if (now.enabled && !m_keys)
    {
        m_keys = std::make_unique<KeyHook>(m_window);
    }
    else if (!now.enabled)
    {
        m_keys.reset();
    }
    if (m_keys)
    {
        m_keys->setKeys(hookKeys());
    }
    if (!now.enabled)
    {
        stop(); // the look alone: every window back where it was
        return;
    }
    if (!old || !old->settings.enabled || old->settings.mode != now.mode)
    {
        stop();
        if (mac() && !m_virtual)
        {
            m_virtual = VirtualDesktops::create();
        }
        if (!m_paused)
        {
            start();
        }
        return;
    }
    if (!m_desktop)
    {
        publish();
        return;
    }
    if (!mac() && old->settings.workspaces != now.workspaces)
    {
        apply(m_desktop->setWorkspaceCount(now.workspaces));
    }
    if (old->settings.layout != now.layout)
    {
        m_desktop->setLayout(now.layout);
    }
    m_bordered.reset(); // the color may have changed: paint it again
    updateBorder(foreground());
    scheduleLayout();
    publish();
}

std::vector<domain::TilingKey> TilerApp::hookKeys() const
{
    std::vector<domain::TilingKey> keys =
        m_plan && m_plan->settings.enabled ? m_plan->settings.keys : std::vector<domain::TilingKey>{};
    if (m_paused)
    {
        // Off for now: every key is the apps' again, but the one that turns it back on.
        std::erase_if(keys, [](const domain::TilingKey& key) { return key.command != TilingCommand::Toggle; });
        return keys;
    }
    if (mac())
    {
        // Windows' own Ctrl+Win+arrows already slide: let them through. Sending a window to another
        // desktop is not something Windows lets a program do: that key stays the app's.
        std::erase_if(keys, [](const domain::TilingKey& key) {
            const bool native = key.keys.modifiers == (MOD_CONTROL | MOD_WIN) &&
                                (key.keys.virtualKey == VK_LEFT || key.keys.virtualKey == VK_RIGHT);
            return key.command == TilingCommand::SendToWorkspace ||
                   (native && (key.command == TilingCommand::WorkspacePrevious ||
                               key.command == TilingCommand::WorkspaceNext));
        });
    }
    return keys;
}

bool TilerApp::mac() const noexcept
{
    return m_plan && m_plan->settings.enabled && m_plan->settings.mode == domain::TilingMode::Mac;
}

void TilerApp::start()
{
    if (!m_plan || !m_plan->settings.enabled || m_desktop)
    {
        return;
    }
    int workspaces = m_plan->settings.workspaces;
    const domain::TilingMode mode = m_plan->settings.mode;
    if (mac() && m_virtual)
    {
        m_virtualState = m_virtual->read();
        workspaces = static_cast<int>(std::min<std::size_t>(m_virtualState.desktops.size(), domain::kMaxWorkspaces));
    }
    m_desktop = std::make_unique<tiling::Desktop>(workspaces, m_plan->settings.layout);
    m_monitors = monitorsNow();
    m_desktop->setMonitors(m_monitors);
    if (mac())
    {
        for (std::size_t m = 0; m < m_desktop->monitorCount(); ++m)
        {
            m_desktop->goTo(m, std::min(m_virtualState.current, workspaces - 1));
        }
    }
    // Bottom to top: the window in front is the newest (and keeps the focus).
    std::vector<HWND> windows;
    EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&windows));
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
    {
        adopt(*it);
    }
    updateBorder(foreground());
    layout();
    publish();
    log::info("tiler: {} window(s) on {} monitor(s), {} style", m_desktop->all().size(), m_monitors.size(),
              domain::toString(mode));
}

void TilerApp::stop()
{
    if (!m_desktop)
    {
        return;
    }
    if (m_animator)
    {
        m_animator->cancelAll(); // a frame after this would take a window from where it goes back to
    }
    std::vector<tiling::Original> windows;
    windows.reserve(m_originals.size());
    for (auto [id, original] : m_originals)
    {
        original.hidden = m_hidden.contains(id);
        windows.push_back(original);
    }
    if (const auto left = putBack(windows); !left.empty())
    {
        log::warn("tiler: {} window(s) of apps running as administrator could not be put back", left.size());
    }
    if (m_bordered)
    {
        setBorder(windowOf(*m_bordered), m_looks.border());
    }
    // Putting them back gave them Windows' border: the look's again.
    for (const auto& original : windows)
    {
        m_looks.paint(windowOf(original.window), true);
    }
    m_originals.clear();
    m_hidden.clear();
    m_followers.clear();
    m_targets.clear();
    m_minimums.clear();
    m_bordered.reset();
    m_pendingFocus.reset();
    m_desktop.reset();
    m_dragFrom.clear();
    save(); // only what waits for an elevated tiler is left: the file goes, or keeps just those
    publish();
}

void TilerApp::remember(tiling::WindowId id, Window window)
{
    if (!m_originals.contains(id))
    {
        m_originals.emplace(id, originalOf(window));
        scheduleSave();
    }
}

void TilerApp::adopt(Window window)
{
    if (!m_desktop || !m_plan)
    {
        return;
    }
    const tiling::WindowId id = idOf(window);
    if (m_desktop->find(id) || !isAppWindow(window) || isOurs(window) || !canMove(window))
    {
        return;
    }
    const tiling::WindowFacts facts = factsOf(window);
    const domain::TilingRule* rule = tiling::matchRule(m_plan->settings.rules, facts);
    if (rule != nullptr && rule->action == domain::TilingRuleAction::Ignore)
    {
        return;
    }
    const bool floating =
        (rule != nullptr && rule->action == domain::TilingRuleAction::Float) || !isResizable(window);
    std::optional<int> workspace;
    if (mac())
    {
        if (!m_virtual)
        {
            return;
        }
        workspace = m_virtual->desktopOf(window, m_virtualState);
        if (!workspace || *workspace >= m_desktop->workspaceCount())
        {
            syncDesktops(); // a desktop made a moment ago, maybe: read them again
            workspace = m_virtual->desktopOf(window, m_virtualState);
        }
        // On no desktop of the list: shown on every one (pinned), so with the one shown now.
        workspace = workspace.value_or(m_virtualState.current);
        if (*workspace >= m_desktop->workspaceCount())
        {
            return; // beyond the ninth
        }
    }
    else if (rule != nullptr && rule->action == domain::TilingRuleAction::Workspace)
    {
        workspace = rule->workspace - 1;
    }
    if (!floating)
    {
        remember(id, window); // before anything moves it
        if (IsZoomed(static_cast<HWND>(window)) != FALSE)
        {
            ShowWindowAsync(static_cast<HWND>(window), SW_SHOWNOACTIVATE); // maximized: back to a tile
        }
    }
    tiling::Change change =
        m_desktop->add(id, monitorIndex(m_monitors, window), workspace, floating);
    if (const auto added = m_desktop->find(id))
    {
        log::info("tiler: window {:#x} ({}, {}) joins workspace {}{}", id, facts.app, facts.windowClass, added->workspace + 1,
                  floating ? " (floating)" : "");
    }
    change.focus.reset(); // a window that opens has the focus it has
    apply(std::move(change));
    scheduleLayout();
    publish();
}

void TilerApp::forget(Window window, bool gone)
{
    const tiling::WindowId id = idOf(window);
    if (gone)
    {
        m_originals.erase(id);
        m_followers.erase(id);
        m_minimums.erase(id); // its handle may name another window later
        m_appearing.erase(id);
        m_dragFrom.erase(id);
        m_hidden.erase(id); // a dialog hidden with its window (not in the model) closed meanwhile
    }
    m_animator->cancel(id);
    if (!m_desktop || !m_desktop->remove(id))
    {
        return;
    }
    log::info("tiler: window {:#x} leaves{}", id, gone ? " (closed)" : "");
    m_hidden.erase(id);
    m_targets.erase(id);
    if (m_bordered == id)
    {
        m_bordered.reset();
    }
    scheduleLayout();
    scheduleSave();
    publish();
}

void TilerApp::apply(tiling::Change change)
{
    if (mac())
    {
        // Windows shows and hides the virtual desktops' windows itself.
        change.hide.clear();
        change.show.clear();
    }
    // The new ones first, then the old ones go: no flash of the empty desktop in between.
    for (const tiling::WindowId id : change.show)
    {
        m_hidden.erase(id);
        if (auto it = m_originals.find(id); it != m_originals.end() && !it->second.placed)
        {
            m_originals.erase(it);
        }
        show(windowOf(id));
        showFollowers(id);
    }
    for (const tiling::WindowId id : change.hide)
    {
        Window window = windowOf(id);
        if (!m_originals.contains(id))
        {
            tiling::Original original = originalOf(window);
            original.placed = false; // a floating window: hidden, never moved
            m_originals.emplace(id, original);
        }
        m_hidden.insert(id);
        m_insets[id] = insetsOf(window); // still shown: measurable
        hideFollowers(id);
        hide(window);
    }
    if (!change.hide.empty() || !change.show.empty())
    {
        save(); // now: a crash right after must find them
    }
    if (change.focus)
    {
        m_pendingFocus = change.focus;
        SetTimer(static_cast<HWND>(m_window), kFocusTimer, change.show.empty() ? 0 : kFocusDelayMs, nullptr);
    }
}

void TilerApp::hideFollowers(tiling::WindowId owner)
{
    FollowerSearch search{static_cast<HWND>(windowOf(owner)), {}};
    EnumWindows(&collectFollower, reinterpret_cast<LPARAM>(&search));
    for (const tiling::WindowId follower : search.found)
    {
        if (!m_originals.contains(follower))
        {
            tiling::Original original = originalOf(windowOf(follower));
            original.placed = false;
            m_originals.emplace(follower, original);
        }
        m_hidden.insert(follower);
        hide(windowOf(follower));
    }
    if (!search.found.empty())
    {
        m_followers[owner] = std::move(search.found);
    }
}

void TilerApp::showFollowers(tiling::WindowId owner)
{
    const auto it = m_followers.find(owner);
    if (it == m_followers.end())
    {
        return;
    }
    for (const tiling::WindowId follower : it->second)
    {
        m_hidden.erase(follower);
        m_originals.erase(follower);
        if (IsWindow(static_cast<HWND>(windowOf(follower))) != FALSE)
        {
            show(windowOf(follower));
        }
    }
    m_followers.erase(it);
}

void TilerApp::scheduleLayout()
{
    SetTimer(static_cast<HWND>(m_window), kLayoutTimer, 0, nullptr);
}

void TilerApp::layout()
{
    m_targets.clear();
    if (!m_desktop || !m_plan)
    {
        return;
    }
    const bool animate = animations();
    std::vector<Animator::Move> moves;
    for (std::size_t m = 0; m < m_desktop->monitorCount(); ++m)
    {
        const tiling::Rect& area = m_desktop->area(m);
        for (const auto& [id, slot] : m_desktop->activeWorkspace(m).placements(area, options(m)))
        {
            Window window = windowOf(id);
            HWND w = static_cast<HWND>(window);
            if (IsWindow(w) == FALSE || IsZoomed(w) != FALSE || IsIconic(w) != FALSE || isFullscreen(window) ||
                m_dragFrom.contains(id))
            {
                continue; // maximized, full screen or held by the person: theirs until they let it go
            }
            remember(id, window);
            const tiling::Rect rect = targetOf(id, slot, area);
            m_targets[id] = rect;
            // Shown in place a moment ago and maybe not shown yet: no visible frame to measure, and a move now
            // would put it off by its borders. settle() looks at it once it shows.
            if (m_justShown.contains(id))
            {
                continue;
            }
            if (const auto going = m_animator->destination(id))
            {
                if (nearly(*going, rect))
                {
                    continue; // on its way there already
                }
                m_animator->cancel(id); // its place changed on the way: from where it is now
            }
            const tiling::Rect frame = frameOf(window);
            if (nearly(frame, rect))
            {
                continue;
            }
            // A window that changes place slides there; one that only changes size gets it at once.
            if (animate && IsWindowVisible(w) != FALSE &&
                (std::abs(frame.left - rect.left) > kSlideMin || std::abs(frame.top - rect.top) > kSlideMin))
            {
                moves.push_back({window, frame, rect, insetsOf(window)});
            }
            else
            {
                place(window, rect);
            }
        }
    }
    m_justShown.clear();
    m_animator->start(std::move(moves), kMoveDuration);
    m_settleRounds = kSettleRounds;
    SetTimer(static_cast<HWND>(m_window), kSettleTimer, kSettleDelayMs, nullptr);
}

tiling::Rect TilerApp::targetOf(tiling::WindowId id, const tiling::Rect& slot, const tiling::Rect& area) const
{
    const auto minimum = m_minimums.find(id);
    return minimum != m_minimums.end() ? tiling::fitted(slot, minimum->second, area) : slot;
}

bool TilerApp::learnMinimum(tiling::WindowId id, const tiling::Rect& frame, const tiling::Rect& slot)
{
    constexpr std::int32_t kSlack = 2;
    if (std::abs(frame.left - slot.left) > kSlack || std::abs(frame.top - slot.top) > kSlack)
    {
        return false; // not where it was put (yet): nothing to learn
    }
    const bool wider = frame.width() > slot.width() + kSlack;
    const bool taller = frame.height() > slot.height() + kSlack;
    if (!wider && !taller)
    {
        return false;
    }
    tiling::Size& minimum = m_minimums[id];
    const tiling::Size before = minimum;
    if (wider)
    {
        minimum.width = std::max(minimum.width, frame.width());
    }
    if (taller)
    {
        minimum.height = std::max(minimum.height, frame.height());
    }
    if (minimum != before)
    {
        log::info("tiler: window {:#x} keeps at least {}x{}; the others make room", id, minimum.width, minimum.height);
        scheduleLayout();
    }
    return true; // never placed again into a slot it refuses (it would refuse at every round)
}

bool TilerApp::animations() const noexcept
{
    return m_plan && m_plan->settings.animations && windowsAnimates();
}

bool TilerApp::onShownDesktop(Window window) const
{
    if (!mac() || !m_virtual)
    {
        return true; // our own workspaces: a window shown is on the screen
    }
    const VirtualDesktops::State now = m_virtual->read();
    const auto desktop = m_virtual->desktopOf(window, now);
    return !desktop || *desktop == now.current; // unknown (pinned to every desktop): fine
}

void TilerApp::placeComing(std::span<const tiling::WindowId> coming, int side)
{
    if (!m_desktop || !m_plan || coming.empty())
    {
        return;
    }
    const bool animate = side != 0 && animations();
    std::vector<Animator::Move> moves;
    for (std::size_t m = 0; m < m_desktop->monitorCount(); ++m)
    {
        const tiling::Rect& area = m_desktop->area(m);
        // Like Hyprland's workspaces, but a short way: the next monitor sees no window pass.
        const std::int32_t offset = side * std::clamp(area.width() / 12, 48, 160);
        for (const auto& [id, slot] : m_desktop->activeWorkspace(m).placements(area, options(m)))
        {
            const auto insets = m_insets.find(id);
            if (std::ranges::find(coming, id) == coming.end() || insets == m_insets.end())
            {
                continue;
            }
            HWND w = static_cast<HWND>(windowOf(id));
            if (IsWindow(w) == FALSE || IsZoomed(w) != FALSE || IsIconic(w) != FALSE || isFullscreen(windowOf(id)))
            {
                continue;
            }
            const tiling::Rect rect = targetOf(id, slot, area);
            if (animate)
            {
                const tiling::Rect from{rect.left + offset, rect.top, rect.right + offset, rect.bottom};
                showInPlace(windowOf(id), from, insets->second);
                moves.push_back({windowOf(id), from, rect, insets->second});
            }
            else
            {
                showInPlace(windowOf(id), rect, insets->second);
            }
            m_justShown.insert(id);
        }
    }
    m_animator->start(std::move(moves), kSwitchDuration);
}

void TilerApp::settle()
{
    // Apps that answer late, or change their size when their DPI changes: placed once more. One that is
    // in its place but larger keeps a size of its own: the others make room for it.
    std::vector<tiling::WindowId> gone;
    for (const auto& [id, rect] : m_targets)
    {
        Window window = windowOf(id);
        HWND w = static_cast<HWND>(window);
        if (IsWindow(w) == FALSE || IsZoomed(w) != FALSE || IsIconic(w) != FALSE || m_animator->moving(id) ||
            isFullscreen(window) || m_dragFrom.contains(id))
        {
            continue;
        }
        if (IsWindowVisible(w) == FALSE && !m_hidden.contains(id))
        {
            gone.push_back(id); // the app hid it while we were showing it (its hide came first): not a slot
            continue;
        }
        const tiling::Rect frame = frameOf(window);
        if (!nearly(frame, rect) && !learnMinimum(id, frame, rect))
        {
            place(window, rect);
        }
    }
    for (const tiling::WindowId id : gone)
    {
        forget(windowOf(id), false);
    }
    if (--m_settleRounds <= 0)
    {
        KillTimer(static_cast<HWND>(m_window), kSettleTimer);
    }
}

tiling::LayoutOptions TilerApp::options(std::size_t monitor) const
{
    const double scale = monitor < m_monitors.size() ? m_monitors[monitor].scale : 1.0;
    tiling::LayoutOptions out = m_plan ? tiling::layoutOptions(m_plan->settings, scale) : tiling::LayoutOptions{};
    out.minimums = &m_minimums;
    return out;
}

std::size_t TilerApp::currentMonitor() const
{
    if (m_desktop)
    {
        if (const auto place = m_desktop->find(idOf(foreground())))
        {
            return place->monitor;
        }
    }
    return monitorIndex(m_monitors, nullptr); // the one under the mouse
}

void TilerApp::onEvent(std::uint32_t event, Window window)
{
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED || event == EVENT_SYSTEM_FOREGROUND)
    {
        m_looks.paint(window);
    }
    else if (event == EVENT_OBJECT_DESTROY)
    {
        m_looks.forget(window);
    }
    if (!m_desktop)
    {
        return;
    }
    const tiling::WindowId id = idOf(window);
    switch (event)
    {
    case EVENT_SYSTEM_FOREGROUND:
        onForeground(window);
        break;
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_UNCLOAKED:
    case EVENT_SYSTEM_MINIMIZEEND:
        // Only top-level windows can be tiled: the rest (a control, a tooltip's parts) is not even waited for.
        if (!m_desktop->find(id) && GetAncestor(static_cast<HWND>(window), GA_ROOT) == window &&
            !m_appearing.contains(id))
        {
            m_appearing.emplace(id, GetTickCount64());
            if (!std::exchange(m_adoptTimer, true))
            {
                SetTimer(static_cast<HWND>(m_window), kAdoptTimer, kAdoptTickMs, nullptr);
            }
        }
        break;
    case EVENT_OBJECT_HIDE:
        // Ours (another workspace), or shown again since: not the app hiding it.
        if (!m_hidden.contains(id) && IsWindowVisible(static_cast<HWND>(window)) == FALSE)
        {
            forget(window, false);
        }
        break;
    case EVENT_OBJECT_CLOAKED:
        // Mac style: on another virtual desktop now, still ours. Linux style: the app went away.
        if (!mac() && !m_hidden.contains(id))
        {
            forget(window, false);
        }
        break;
    case EVENT_SYSTEM_MINIMIZESTART:
        forget(window, false);
        break;
    case EVENT_OBJECT_DESTROY:
        forget(window, true);
        break;
    case EVENT_SYSTEM_MOVESIZESTART:
        m_animator->cancel(id); // the person has it now: nothing moves it meanwhile
        m_dragFrom[id] = frameOf(window);
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        onMoved(window);
        break;
    default:
        break;
    }
}

void TilerApp::onForeground(Window window)
{
    const tiling::WindowId id = idOf(window);
    auto place = m_desktop->find(id);
    if (!place)
    {
        adopt(window);
        place = m_desktop->find(id);
    }
    if (place)
    {
        m_desktop->workspace(*place).focus(id);
        // An app brought a window of a workspace not shown to the front: that workspace comes. Not right
        // after our own switch: a window we were hiding took the front for an instant (Windows gives it to
        // the next window as the focused one hides) and the switch bounced back.
        if (!mac() && !m_desktop->shown(id) && IsWindowVisible(static_cast<HWND>(window)) != FALSE &&
            GetTickCount64() - m_switchedAt > kSwitchCalmMs)
        {
            log::info("tiler: window {:#x} of workspace {} came to the front; going there", id, place->workspace + 1);
            goTo(place->monitor, place->workspace);
        }
    }
    updateBorder(window);
}

void TilerApp::onMoved(Window window)
{
    const tiling::WindowId id = idOf(window);
    const auto from = m_dragFrom.find(id);
    std::optional<tiling::Rect> before;
    if (from != m_dragFrom.end())
    {
        before = from->second;
        m_dragFrom.erase(from);
    }
    const auto place = m_desktop->find(id);
    if (!place || m_desktop->workspace(*place).floating(id))
    {
        return;
    }
    // Its size changed: an edge was dragged, not the window. Back to its slot (the cursor may well be over
    // the neighbour, which a move would swap it with).
    if (const tiling::Rect after = frameOf(window);
        before && (std::abs(after.width() - before->width()) > kSlideMin ||
                   std::abs(after.height() - before->height()) > kSlideMin))
    {
        scheduleLayout();
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    const std::size_t monitor = monitorIndex(m_monitors, nullptr);
    if (monitor != place->monitor)
    {
        tiling::Change change = m_desktop->moveToMonitor(id, monitor);
        change.focus.reset();
        apply(std::move(change));
    }
    else
    {
        // Dropped onto another window: they trade places. Anywhere else: back to its tile.
        tiling::Workspace& workspace = m_desktop->workspace(*place);
        for (const auto& [other, rect] : workspace.placements(m_desktop->area(monitor), options(monitor)))
        {
            if (other != id && rect.contains(cursor.x, cursor.y))
            {
                workspace.exchange(id, other);
                break;
            }
        }
    }
    scheduleLayout();
    publish();
}

void TilerApp::command(TilingCommand command, int number)
{
    log::info("tiler: {}{}", domain::toString(command), number != 0 ? std::format(" {}", number) : std::string());
    if (m_desktop && m_desktop->monitorCount() == 0)
    {
        return; // no screen at all (all off, a remote session starting): nothing to move or show
    }
    if (command == TilingCommand::Toggle)
    {
        m_paused = m_desktop != nullptr;
        if (m_paused)
        {
            log::info("tiler: off for now; every window back");
            stop();
        }
        else
        {
            start();
        }
        if (m_keys)
        {
            m_keys->setKeys(hookKeys());
        }
        publish();
        return;
    }
    if (!m_desktop)
    {
        return;
    }
    if (const auto direction = directionOf(command))
    {
        if (command == TilingCommand::MoveLeft || command == TilingCommand::MoveRight ||
            command == TilingCommand::MoveUp || command == TilingCommand::MoveDown)
        {
            moveToward(*direction);
        }
        else
        {
            focusToward(*direction);
        }
        return;
    }
    auto* const window = foreground();
    const tiling::WindowId id = idOf(window);
    const auto place = m_desktop->find(id);
    const std::size_t monitor = place ? place->monitor : currentMonitor();
    switch (command)
    {
    case TilingCommand::WorkspacePrevious:
    case TilingCommand::WorkspaceNext: {
        const bool next = command == TilingCommand::WorkspaceNext;
        if (mac())
        {
            VirtualDesktops::step(next);
            break;
        }
        const int count = m_desktop->workspaceCount();
        goTo(monitor, (m_desktop->active(monitor) + (next ? 1 : count - 1)) % count, next ? 1 : -1);
        break;
    }
    case TilingCommand::Workspace:
        // A new switch: the focus the last one was going to give is not wanted anymore.
        m_pendingFocus.reset();
        KillTimer(static_cast<HWND>(m_window), kFocusTimer);
        if (mac())
        {
            // As Windows has it now: a switch a moment ago may not have reached us yet, and a stale
            // "current" sends no keys at all. (A copy: syncDesktops() still sees the change.)
            VirtualDesktops::goTo(number - 1, m_virtual ? m_virtual->read() : m_virtualState);
        }
        else
        {
            goTo(monitor, number - 1);
        }
        break;
    case TilingCommand::SendToWorkspace:
        if (mac())
        {
            if (!std::exchange(m_toldMacSend, true))
            {
                log::info("tiler: Windows does not let a program move another app's window to another "
                          "virtual desktop; use Win+Tab");
            }
        }
        else if (place)
        {
            apply(m_desktop->sendTo(id, number - 1));
            scheduleLayout();
            publish();
        }
        break;
    case TilingCommand::Float:
        if (place)
        {
            tiling::Workspace& workspace = m_desktop->workspace(*place);
            workspace.toggleFloating(id);
            if (!workspace.floating(id))
            {
                remember(id, window);
            }
            scheduleLayout();
        }
        break;
    case TilingCommand::Fullscreen:
        m_desktop->activeWorkspace(monitor).toggleMonocle();
        scheduleLayout();
        publish();
        break;
    case TilingCommand::NextLayout:
        m_desktop->activeWorkspace(monitor).cycleLayout();
        scheduleLayout();
        publish();
        break;
    case TilingCommand::Promote:
        if (place)
        {
            m_desktop->workspace(*place).promote(id);
            scheduleLayout();
        }
        break;
    default:
        break;
    }
}

void TilerApp::focusToward(tiling::Direction direction)
{
    const tiling::WindowId id = idOf(foreground());
    const auto place = m_desktop->find(id);
    const std::size_t monitor = place ? place->monitor : currentMonitor();
    if (place && m_desktop->shown(id))
    {
        const tiling::Workspace& workspace = m_desktop->workspace(*place);
        if (const auto next = workspace.neighborOf(id, direction, m_desktop->area(monitor), options(monitor)))
        {
            focus(windowOf(*next));
            return;
        }
    }
    // Past the last window: the monitor that way (or, from a window not ours, this monitor's).
    const auto other = place ? m_desktop->monitorToward(monitor, direction) : std::optional(monitor);
    if (!other)
    {
        return;
    }
    if (const auto focused = m_desktop->activeWorkspace(*other).focused())
    {
        focus(windowOf(*focused));
    }
    else
    {
        // Nothing there: the mouse goes, so the next window opens on it.
        const tiling::Rect& area = m_desktop->area(*other);
        SetCursorPos((area.left + area.right) / 2, (area.top + area.bottom) / 2);
    }
}

void TilerApp::moveToward(tiling::Direction direction)
{
    const tiling::WindowId id = idOf(foreground());
    const auto place = m_desktop->find(id);
    if (!place || !m_desktop->shown(id) || m_desktop->workspace(*place).floating(id))
    {
        return;
    }
    tiling::Workspace& workspace = m_desktop->workspace(*place);
    if (!workspace.exchange(id, direction, m_desktop->area(place->monitor), options(place->monitor)))
    {
        if (const auto other = m_desktop->monitorToward(place->monitor, direction))
        {
            tiling::Change change = m_desktop->moveToMonitor(id, *other);
            change.focus.reset();
            apply(std::move(change));
        }
    }
    scheduleLayout();
    publish();
}

void TilerApp::goTo(std::size_t monitor, int workspace, int side)
{
    if (!m_desktop)
    {
        return;
    }
    if (side == 0 && monitor < m_desktop->monitorCount())
    {
        const int now = m_desktop->active(monitor);
        side = workspace > now ? 1 : workspace < now ? -1 : 0; // a higher number is to the right
    }
    tiling::Change change = m_desktop->goTo(monitor, workspace);
    m_switchedAt = GetTickCount64();
    if (!mac())
    {
        // In their slots and on top before the old ones go (v1.6: they showed under the old ones, where
        // they had been, and moved after: a cascade and a jolt at every switch). With animations, they
        // slide in from the side the workspace is on.
        placeComing(change.show, side);
    }
    apply(std::move(change));
    layout(); // whatever is not in its place yet (a window that came from another monitor)
    publish();
}

void TilerApp::syncMonitors()
{
    if (!m_desktop)
    {
        return;
    }
    m_monitors = monitorsNow();
    apply(m_desktop->setMonitors(m_monitors));
    scheduleLayout();
    publish();
}

void TilerApp::syncDesktops()
{
    if (!m_desktop || !m_virtual || !mac())
    {
        return;
    }
    const VirtualDesktops::State before = m_virtualState;
    m_virtualState = m_virtual->read();
    const int count = static_cast<int>(std::min<std::size_t>(m_virtualState.desktops.size(), domain::kMaxWorkspaces));
    if (m_virtualState.desktops.size() != before.desktops.size() || m_virtualState.current != before.current)
    {
        log::info("tiler: {} virtual desktop(s), number {} shown", m_virtualState.desktops.size(),
                  m_virtualState.current + 1);
    }
    m_desktop->setWorkspaceCount(count);
    // Windows moved to another desktop with Win+Tab: they follow.
    for (const tiling::WindowId id : m_desktop->all())
    {
        const auto desktop = m_virtual->desktopOf(windowOf(id), m_virtualState);
        const auto place = m_desktop->find(id);
        if (desktop && place && *desktop != place->workspace && *desktop < count)
        {
            m_desktop->sendTo(id, *desktop);
        }
    }
    for (std::size_t m = 0; m < m_desktop->monitorCount(); ++m)
    {
        m_desktop->goTo(m, std::min(m_virtualState.current, count - 1));
    }
    scheduleLayout();
    publish();
}

void TilerApp::updateBorder(Window focused)
{
    const tiling::WindowId id = idOf(focused);
    std::optional<std::uint32_t> color;
    if (m_plan && m_plan->settings.border && m_desktop && m_desktop->find(id))
    {
        color = colorref(m_plan->border);
    }
    if (m_bordered && (!color || *m_bordered != id))
    {
        setBorder(windowOf(*m_bordered), m_looks.border()); // the look's border, or Windows'
        m_bordered.reset();
    }
    if (color && m_bordered != id)
    {
        setBorder(focused, color);
        m_bordered = id;
    }
}

void TilerApp::scheduleSave()
{
    SetTimer(static_cast<HWND>(m_window), kSaveTimer, 300, nullptr);
}

void TilerApp::save()
{
    std::error_code ec;
    // An admin app's windows a past tiler hid stay listed until a tiler that may move them puts them back.
    std::erase_if(m_waiting, [](const tiling::Original& original) {
        return IsWindow(static_cast<HWND>(windowOf(original.window))) == FALSE;
    });
    if (m_originals.empty() && m_waiting.empty())
    {
        std::filesystem::remove(m_windowsFile, ec);
        return;
    }
    std::vector<tiling::Original> windows;
    windows.reserve(m_originals.size() + m_waiting.size());
    for (auto [id, original] : m_originals)
    {
        original.hidden = m_hidden.contains(id);
        windows.push_back(original);
    }
    for (const auto& waiting : m_waiting)
    {
        if (!m_originals.contains(waiting.window))
        {
            windows.push_back(waiting);
        }
    }
    const std::string text = tiling::toJson(windows);
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    // Not flushed to the disk: it is written at every switch, and a crash of the tiler (what it is for)
    // leaves it in Windows' cache all the same.
    if (auto written = platform::writeFileAtomic(m_windowsFile, std::span(bytes, text.size()),
                                                 platform::Durability::Cached);
        !written)
    {
        log::warn("tiler: cannot keep the list of windows to put back: {}", written.error().toString());
    }
}

void TilerApp::publish()
{
    m_shared.write([this](tiling::SharedState& state) {
        state.mode = static_cast<std::uint8_t>(m_plan ? m_plan->settings.mode : domain::TilingMode::Linux);
        state.paused = m_desktop ? 0 : 1;
        state.workspaces = m_desktop ? static_cast<std::uint32_t>(m_desktop->workspaceCount()) : 0;
        const std::size_t count = m_desktop ? std::min(m_desktop->monitorCount(), tiling::kMaxSharedMonitors) : 0;
        state.monitorCount = static_cast<std::uint32_t>(count);
        for (std::size_t m = 0; m < count; ++m)
        {
            tiling::SharedMonitor& out = state.monitors.at(m);
            const tiling::Rect bounds = m < m_monitors.size() ? m_monitors[m].bounds : tiling::Rect{};
            out.left = bounds.left;
            out.top = bounds.top;
            out.right = bounds.right;
            out.bottom = bounds.bottom;
            out.occupied = m_desktop->occupied(m);
            out.active = static_cast<std::uint8_t>(m_desktop->active(m));
            out.layout = static_cast<std::uint8_t>(m_desktop->activeWorkspace(m).layout());
        }
    });
}

} // namespace rmw::tilerapp
