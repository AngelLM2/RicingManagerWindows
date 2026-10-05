#include "SlintPlatform.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <algorithm>

namespace rmw::barapp
{
namespace
{

constexpr UINT kRunTasks = WM_APP + 1;
constexpr UINT kQuit = WM_APP + 2;
constexpr const wchar_t* kClassName = L"RicingManagerWindows.BarLoop";

} // namespace

struct Win32Platform::Access
{
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Win32Platform*>( // NOLINT(performance-no-int-to-ptr)
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr && message == kRunTasks)
        {
            self->runTasks();
            return 0;
        }
        if (self != nullptr && message == kQuit)
        {
            self->m_running = false;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

Win32Platform::Win32Platform(AdapterFactory factory)
    : m_factory(std::move(factory))
{
    s_current = this;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Access::proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window != nullptr)
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    m_messages = window;
}

Win32Platform::~Win32Platform()
{
    s_current = nullptr;
    if (m_messages != nullptr)
    {
        SetWindowLongPtrW(static_cast<HWND>(m_messages), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_messages));
    }
}

std::unique_ptr<slint::platform::WindowAdapter> Win32Platform::create_window_adapter()
{
    return m_factory();
}

void Win32Platform::watch(void* handle, std::function<void()> wake)
{
    unwatch(handle);
    m_watches.push_back({handle, std::move(wake)});
}

void Win32Platform::unwatch(void* handle)
{
    std::erase_if(m_watches, [&](const Watch& watch) { return watch.handle == handle; });
}

void Win32Platform::everyTurn(std::function<void()> turn, std::chrono::milliseconds period)
{
    m_turn = std::move(turn);
    m_period = period;
}

void Win32Platform::run_event_loop()
{
    m_running = true;
    std::vector<HANDLE> handles;
    while (m_running)
    {
        slint::platform::update_timers_and_animations();
        if (m_turn)
        {
            m_turn();
        }
        auto timeout = m_period;
        if (const auto next = slint::platform::duration_until_next_timer_update())
        {
            timeout = std::clamp(*next, std::chrono::milliseconds(0), timeout);
        }
        handles.clear();
        for (const auto& watch : m_watches)
        {
            handles.push_back(watch.handle);
        }
        const auto count = static_cast<DWORD>(handles.size());
        const DWORD woken = MsgWaitForMultipleObjectsEx(
            count, handles.data(), static_cast<DWORD>(timeout.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (woken < WAIT_OBJECT_0 + count) // WAIT_OBJECT_0 is 0
        {
            const auto wake = m_watches[woken - WAIT_OBJECT_0].wake; // a copy: it may unwatch
            wake();
            continue;
        }
        if (woken == WAIT_FAILED)
        {
            log::error("bar loop: wait failed ({})", GetLastError());
            return;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                m_running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void Win32Platform::quit_event_loop()
{
    PostMessageW(static_cast<HWND>(m_messages), kQuit, 0, 0);
}

void Win32Platform::run_in_event_loop(Task task)
{
    {
        const std::scoped_lock lock(m_tasksMutex);
        m_tasks.push_back(std::move(task));
    }
    PostMessageW(static_cast<HWND>(m_messages), kRunTasks, 0, 0);
}

void Win32Platform::runTasks()
{
    std::deque<Task> tasks;
    {
        const std::scoped_lock lock(m_tasksMutex);
        tasks.swap(m_tasks);
    }
    for (auto& task : tasks)
    {
        std::move(task).run();
    }
}

} // namespace rmw::barapp
