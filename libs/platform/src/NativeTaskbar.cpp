#include "rmw/platform/NativeTaskbar.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>

namespace rmw::platform
{
namespace
{

std::atomic<NativeTaskbarHider*> g_hider{nullptr}; // WinEvent callbacks carry no context

std::vector<HWND> taskbarWindows()
{
    std::vector<HWND> windows;
    if (auto* const primary = FindWindowW(L"Shell_TrayWnd", nullptr))
    {
        windows.push_back(primary);
    }
    EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            wchar_t name[64] = {}; // NOLINT(modernize-avoid-c-arrays): GetClassName's buffer
            if (GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 &&
                lstrcmpW(name, L"Shell_SecondaryTrayWnd") == 0)
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands our pointer back
                reinterpret_cast<std::vector<HWND>*>(list)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

} // namespace

void showNativeTaskbars() noexcept
{
    try
    {
        for (auto* const window : taskbarWindows())
        {
            ShowWindowAsync(window, SW_SHOWNA);
        }
    }
    catch (...)
    {
        // never throws: only the list's allocation could fail, and then there is nothing to show
    }
}

struct NativeTaskbarHider::Access
{
    static void CALLBACK onShow(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
    {
        if (object != OBJID_WINDOW)
        {
            return;
        }
        if (auto* hider = g_hider.load())
        {
            hider->rehide(window);
        }
    }
};

NativeTaskbarHider::~NativeTaskbarHider()
{
    show();
}

void NativeTaskbarHider::hide(std::vector<void*> monitors)
{
    show();
    m_monitors = std::move(monitors);
    if (m_monitors.empty())
    {
        return;
    }
    for (auto* const window : taskbarWindows())
    {
        auto* const monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (std::ranges::find(m_monitors, static_cast<void*>(monitor)) != m_monitors.end())
        {
            ShowWindowAsync(window, SW_HIDE);
            m_hidden.push_back(window);
        }
    }
    if (m_hidden.empty())
    {
        return;
    }
    g_hider = this;
    DWORD explorer = 0;
    GetWindowThreadProcessId(static_cast<HWND>(m_hidden.front()), &explorer);
    m_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &Access::onShow, explorer, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (m_hook == nullptr)
    {
        log::warn("native taskbar: cannot watch it ({}); it may come back until the bar restarts",
                  GetLastError());
    }
}

void NativeTaskbarHider::rehide(void* window) noexcept
{
    if (std::ranges::find(m_hidden, window) != m_hidden.end())
    {
        ShowWindowAsync(static_cast<HWND>(window), SW_HIDE);
    }
}

void NativeTaskbarHider::show() noexcept
{
    if (m_hook != nullptr)
    {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hook));
        m_hook = nullptr;
    }
    NativeTaskbarHider* self = this;
    g_hider.compare_exchange_strong(self, nullptr);
    for (void* window : m_hidden)
    {
        ShowWindowAsync(static_cast<HWND>(window), SW_SHOWNA);
    }
    m_hidden.clear();
}

} // namespace rmw::platform
