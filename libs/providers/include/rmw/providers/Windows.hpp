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

// An icon's pixels: straight (not premultiplied) RGBA, row by row, top first.
struct IconPixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool empty() const noexcept { return rgba.empty(); }
    friend bool operator==(const IconPixels&, const IconPixels&) = default;
};

// Draws an HICON at `size` x `size` pixels; 0 takes the icon's own size (at most 64). Icons without
// an alpha channel take it from their mask. Any thread: an HICON is usable from any process of the
// session, and the copy stays valid after its owner destroys it. Empty when it cannot be drawn.
[[nodiscard]] IconPixels iconPixels(void* icon, int size = 0);

// Who a window belongs to, as Start knows it: its AppUserModelID (packaged and grouped apps, File
// Explorer) or else its executable's path. Windows of one app share it. "" when it cannot be read.
[[nodiscard]] std::wstring appIdentity(void* window);
// The app's icon as the Shell draws it (Start, the taskbar), at `size` pixels or a little more:
// sharp at any scale, where a window's own icon is often 16 px. COM must be initialized on the
// calling thread. Empty when there is none.
[[nodiscard]] IconPixels appIconPixels(const std::wstring& identity, int size);

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
