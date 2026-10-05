#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The windows a taskbar shows: the Alt+Tab rules (visible, unowned or app windows, not tool
// windows, not cloaked — so not those of other virtual desktops). Windows are HWNDs kept as
// void* so callers need not include <windows.h>; across processes they travel as integers.
namespace rmw::providers
{

[[nodiscard]] bool isTaskbarWindow(void* window);
[[nodiscard]] std::string windowTitle(void* window);
// The executable's file name ("firefox.exe"), or "" when the process cannot be asked.
[[nodiscard]] std::string windowApp(void* window);

// Taskbar windows, top of the z-order first.
[[nodiscard]] std::vector<void*> taskbarWindows();

// What a taskbar button does: brings it forward, or minimizes it when it already is in front.
void activateWindow(void* window);
// Brings it forward (restoring it when minimized).
void focusWindow(void* window);
void minimizeWindow(void* window);
// Asks it to close (WM_CLOSE: the app may ask to save first).
void closeWindow(void* window);

// WinEvent hooks out of context (nothing of ours goes into other processes) for what changes a
// taskbar: foreground, minimize, show/hide/destroy, title, cloak. `changed` runs on the thread that
// created the hooks, from its message loop. One set per process.
class WindowEventHooks
{
public:
    explicit WindowEventHooks(void (*changed)());
    ~WindowEventHooks();
    WindowEventHooks(const WindowEventHooks&) = delete;
    WindowEventHooks& operator=(const WindowEventHooks&) = delete;
    WindowEventHooks(WindowEventHooks&&) = delete;
    WindowEventHooks& operator=(WindowEventHooks&&) = delete;

private:
    std::vector<void*> m_hooks; // HWINEVENTHOOK
};

} // namespace rmw::providers
