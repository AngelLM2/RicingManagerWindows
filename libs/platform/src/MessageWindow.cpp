#include "rmw/platform/MessageWindow.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shellapi.h>

#include <dbt.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace rmw::platform
{

namespace
{

constexpr const wchar_t* kClassName = L"RicingManagerWindows.CoreWindow";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kRunTasks = WM_APP + 2;
constexpr UINT kQuit = WM_APP + 3;
constexpr UINT kTrayIconId = 1;

std::uint32_t explorerPid() noexcept
{
    DWORD pid = 0;
    if (const auto tray = FindWindowW(L"Shell_TrayWnd", nullptr))
    {
        GetWindowThreadProcessId(tray, &pid);
    }
    return pid;
}

} // namespace

struct MessageWindowAccess
{
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lParam); // NOLINT(performance-no-int-to-ptr)
            auto* creating = static_cast<MessageWindow*>(create->lpCreateParams);
            creating->m_window = window; // messages arrive before CreateWindowEx returns
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating));
        }
        auto* self = reinterpret_cast<MessageWindow*>( // NOLINT(performance-no-int-to-ptr)
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        // Nothing may unwind into the system's dispatcher.
        try
        {
            return self->handle(message, wParam, lParam);
        }
        catch (const std::exception& e)
        {
            log::error("window message {} failed: {}", message, e.what());
            return 0;
        }
    }
};

Result<std::unique_ptr<MessageWindow>> MessageWindow::create(Handler handler)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &MessageWindowAccess::windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return std::unexpected(lastError("RegisterClassEx"));
    }

    auto self = std::unique_ptr<MessageWindow>(new MessageWindow());
    self->m_handler = std::move(handler);
    self->m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    self->m_explorerPid = explorerPid();
    self->m_icon = LoadIconW(nullptr, IDI_APPLICATION);
    // WS_EX_TOOLWINDOW keeps it off Alt+Tab even if something shows it.
    const auto window = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Ricing Manager", WS_POPUP, 0, 0, 0, 0,
                                        nullptr, nullptr, instance, self.get());
    if (window == nullptr)
    {
        return std::unexpected(lastError("CreateWindowEx"));
    }
    self->m_window = window;
    // Explorer and TaskbarCreated come from lower-integrity processes in some setups.
    ChangeWindowMessageFilterEx(window, self->m_taskbarCreated, MSGFLT_ALLOW, nullptr);

    self->m_powerNotification =
        RegisterPowerSettingNotification(window, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    self->m_sessionNotification = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) != FALSE;
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
    self->m_deviceNotification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    return self;
}

MessageWindow::~MessageWindow()
{
    unregisterHotkeys();
    hideTray();
    if (m_powerNotification != nullptr)
    {
        UnregisterPowerSettingNotification(m_powerNotification);
    }
    if (m_deviceNotification != nullptr)
    {
        UnregisterDeviceNotification(m_deviceNotification);
    }
    for (const auto& watch : m_watches)
    {
        FindCloseChangeNotification(watch.handle);
    }
    if (m_window != nullptr)
    {
        if (m_sessionNotification)
        {
            WTSUnRegisterSessionNotification(static_cast<HWND>(m_window));
        }
        SetWindowLongPtrW(static_cast<HWND>(m_window), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_window));
    }
}

void MessageWindow::emit(const SystemEvent& event)
{
    if (m_handler)
    {
        m_handler(event);
    }
}

std::intptr_t MessageWindow::handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
    const auto window = static_cast<HWND>(m_window);
    if (message == m_taskbarCreated && m_taskbarCreated != 0)
    {
        if (m_trayShown)
        {
            addTrayIcon(); // the old icon died with the old Explorer
        }
        // A DPI change of the main monitor sends it too: only a new Explorer is a restart.
        const std::uint32_t pid = explorerPid();
        if (pid != m_explorerPid)
        {
            m_explorerPid = pid;
            emit({SystemEventKind::ExplorerRestarted});
        }
        return 0;
    }
    switch (message)
    {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        emit({SystemEventKind::DisplayChanged});
        return 0;
    case WM_SETTINGCHANGE: {
        const auto* area = reinterpret_cast<const wchar_t*>(lParam); // NOLINT(performance-no-int-to-ptr)
        if (wParam == SPI_SETDESKWALLPAPER)
        {
            emit({SystemEventKind::WallpaperChanged});
        }
        else if (wParam == SPI_SETCURSORS)
        {
            emit({SystemEventKind::CursorsChanged});
        }
        else if (wParam == SPI_SETWORKAREA)
        {
            emit({SystemEventKind::DisplayChanged});
        }
        else if (area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0)
        {
            emit({SystemEventKind::ThemeChanged});
        }
        else if (area != nullptr && lstrcmpiW(area, L"TraySettings") == 0)
        {
            emit({SystemEventKind::TaskbarChanged});
        }
        return 0;
    }
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        emit({SystemEventKind::ThemeChanged});
        return 0;
    case WM_TIMECHANGE:
        emit({SystemEventKind::TimeChanged});
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC)
        {
            emit({SystemEventKind::Resumed});
        }
        else if (wParam == PBT_POWERSETTINGCHANGE)
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (setting != nullptr && setting->PowerSetting == GUID_ACDC_POWER_SOURCE &&
                setting->DataLength >= sizeof(DWORD))
            {
                DWORD source = 0;
                std::memcpy(&source, setting->Data, sizeof(source));
                emit({SystemEventKind::PowerSource, 0, source == 0 /* PoAc */});
            }
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK)
        {
            emit({SystemEventKind::SessionLocked});
        }
        else if (wParam == WTS_SESSION_UNLOCK)
        {
            emit({SystemEventKind::SessionUnlocked});
        }
        return 0;
    case WM_DEVICECHANGE: {
        // Volumes and ports are broadcast to every window too: only our monitor filter counts.
        const auto* header =
            reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam); // NOLINT(performance-no-int-to-ptr)
        if (wParam == DBT_DEVICEARRIVAL && header != nullptr &&
            header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
            reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(header)->dbcc_classguid ==
                GUID_DEVINTERFACE_MONITOR)
        {
            emit({SystemEventKind::MonitorArrived});
        }
        return TRUE;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam != FALSE)
        {
            emit({SystemEventKind::SessionEnding});
        }
        return 0;
    case WM_HOTKEY:
        emit({SystemEventKind::Hotkey, static_cast<std::uint32_t>(wParam)});
        return 0;
    case WM_TIMER:
        KillTimer(window, wParam);
        emit({SystemEventKind::Timer, static_cast<std::uint32_t>(wParam)});
        return 0;
    case WM_COMMAND:
        emit({SystemEventKind::TrayCommand, LOWORD(wParam)});
        return 0;
    case kTrayCallback:
        // NOTIFYICON_VERSION_4: the event is in LOWORD(lParam).
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP ||
            LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT)
        {
            showMenu();
        }
        return 0;
    case kRunTasks: {
        std::vector<std::function<void()>> tasks;
        {
            const std::scoped_lock lock(m_tasksMutex);
            tasks.swap(m_tasks);
        }
        for (auto& task : tasks)
        {
            task();
        }
        return 0;
    }
    case kQuit:
        PostQuitMessage(static_cast<int>(wParam));
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

Result<void> MessageWindow::registerHotkey(std::uint32_t id, std::uint32_t modifiers,
                                           std::uint32_t virtualKey)
{
    if (RegisterHotKey(static_cast<HWND>(m_window), static_cast<int>(id), modifiers | MOD_NOREPEAT,
                       virtualKey) == FALSE)
    {
        // Usually another program owns the combination.
        return std::unexpected(lastError("RegisterHotKey"));
    }
    m_hotkeys.push_back(id);
    return {};
}

void MessageWindow::unregisterHotkeys() noexcept
{
    for (const auto id : m_hotkeys)
    {
        UnregisterHotKey(static_cast<HWND>(m_window), static_cast<int>(id));
    }
    m_hotkeys.clear();
}

void MessageWindow::addTrayIcon() noexcept
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(m_window);
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallback;
    data.hIcon = static_cast<HICON>(m_icon);
    m_tooltip.copy(data.szTip, std::min<std::size_t>(m_tooltip.size(), std::size(data.szTip) - 1));
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

Result<void> MessageWindow::showTray(std::wstring_view tooltip, std::vector<TrayItem> menu)
{
    m_tooltip = tooltip;
    m_menu = std::move(menu);
    if (!m_trayShown)
    {
        addTrayIcon();
        m_trayShown = true;
    }
    return {};
}

void MessageWindow::hideTray() noexcept
{
    if (!m_trayShown)
    {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(m_window);
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    m_trayShown = false;
}

void MessageWindow::notify(std::wstring_view title, std::wstring_view text) noexcept
{
    if (!m_trayShown)
    {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(m_window);
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    title.copy(data.szInfoTitle, std::min<std::size_t>(title.size(), std::size(data.szInfoTitle) - 1));
    text.copy(data.szInfo, std::min<std::size_t>(text.size(), std::size(data.szInfo) - 1));
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MessageWindow::showMenu()
{
    const auto menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }
    for (const auto& item : m_menu)
    {
        if (item.id == 0)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const UINT flags = MF_STRING | (item.checked ? MF_CHECKED : 0) | (item.enabled ? 0 : MF_GRAYED);
        AppendMenuW(menu, flags, item.id, item.label.c_str());
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    // Required for the menu to close when the user clicks elsewhere.
    SetForegroundWindow(static_cast<HWND>(m_window));
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, static_cast<HWND>(m_window),
                     nullptr);
    DestroyMenu(menu);
}

void MessageWindow::setTimer(std::uint32_t id, std::chrono::milliseconds delay) noexcept
{
    const auto ms =
        static_cast<UINT>(std::clamp<std::int64_t>(delay.count(), USER_TIMER_MINIMUM, 0x7FFFFFFF));
    SetTimer(static_cast<HWND>(m_window), id, ms, nullptr);
}

void MessageWindow::killTimer(std::uint32_t id) noexcept
{
    KillTimer(static_cast<HWND>(m_window), id);
}

Result<void> MessageWindow::watchFolder(std::uint32_t id, const std::filesystem::path& folder)
{
    const auto handle = FindFirstChangeNotificationW(
        folder.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return std::unexpected(lastError("FindFirstChangeNotification"));
    }
    if (m_watches.size() + 1 >= MAXIMUM_WAIT_OBJECTS)
    {
        FindCloseChangeNotification(handle);
        return fail(ErrorCode::LimitExceeded, "too many folder watches");
    }
    m_watches.push_back({id, handle});
    return {};
}

int MessageWindow::run()
{
    std::vector<HANDLE> handles;
    while (true)
    {
        handles.clear();
        for (const auto& watch : m_watches)
        {
            handles.push_back(watch.handle);
        }
        const auto count = static_cast<DWORD>(handles.size());
        const DWORD woken =
            MsgWaitForMultipleObjectsEx(count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (woken < WAIT_OBJECT_0 + count) // WAIT_OBJECT_0 is 0
        {
            // Re-arm before telling anyone: the handler may add watches.
            const Watch fired = m_watches[woken - WAIT_OBJECT_0];
            FindNextChangeNotification(fired.handle);
            emit({SystemEventKind::FolderChanged, fired.id});
            continue;
        }
        if (woken == WAIT_FAILED)
        {
            log::error("message loop: {}", lastError("MsgWaitForMultipleObjectsEx").toString());
            return 1;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                return static_cast<int>(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void MessageWindow::post(std::function<void()> task)
{
    {
        const std::scoped_lock lock(m_tasksMutex);
        m_tasks.push_back(std::move(task));
    }
    PostMessageW(static_cast<HWND>(m_window), kRunTasks, 0, 0);
}

void MessageWindow::quit(int exitCode) noexcept
{
    PostMessageW(static_cast<HWND>(m_window), kQuit, static_cast<WPARAM>(exitCode), 0);
}

} // namespace rmw::platform
