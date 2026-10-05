#pragma once

#include "rmw/base/Error.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The core's window (docs/03 §9): hidden and top-level — a message-only window would miss
// broadcasts such as WM_DISPLAYCHANGE and TaskbarCreated. Everything it hears becomes a
// SystemEvent for the handler, on the window's thread. The handler must not block.
namespace rmw::platform
{

enum class SystemEventKind
{
    ExplorerRestarted, // TaskbarCreated from a new Explorer process
    DisplayChanged,    // resolution, DPI, monitors added/removed (re-enumerate after a short delay)
    ThemeChanged,      // ImmersiveColorSet / colorization changed
    CursorsChanged,    // SPI_SETCURSORS broadcast
    WallpaperChanged,  // SPI_SETDESKWALLPAPER broadcast
    TaskbarChanged,    // TraySettings broadcast
    PowerSource,       // plugged in / on battery (see onAc)
    Resumed,           // back from sleep
    SessionLocked,
    SessionUnlocked,
    TimeChanged, // the clock or time zone changed
    MonitorArrived,
    Hotkey,        // id = hotkey id
    TrayCommand,   // id = menu item id
    Timer,         // id = timer id
    FolderChanged, // id = watch id (something in a watched folder was written)
    SessionEnding, // log off / shut down: the process may be killed once the handler returns
};

struct SystemEvent
{
    SystemEventKind kind = SystemEventKind::Timer;
    std::uint32_t id = 0;
    bool onAc = true;
};

struct TrayItem
{
    std::uint32_t id = 0; // 0 = separator
    std::wstring label;
    bool checked = false;
    bool enabled = true;
};

class MessageWindow
{
public:
    using Handler = std::function<void(const SystemEvent&)>;

    // Creates the window on the calling thread, which must then call run().
    [[nodiscard]] static Result<std::unique_ptr<MessageWindow>> create(Handler handler);
    ~MessageWindow();
    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;
    MessageWindow(MessageWindow&&) = delete;
    MessageWindow& operator=(MessageWindow&&) = delete;

    // Window thread only.
    [[nodiscard]] Result<void> registerHotkey(std::uint32_t id, std::uint32_t modifiers,
                                              std::uint32_t virtualKey);
    void unregisterHotkeys() noexcept;
    void unregisterHotkey(std::uint32_t id) noexcept;
    [[nodiscard]] Result<void> showTray(std::wstring_view tooltip, std::vector<TrayItem> menu);
    void hideTray() noexcept;
    void notify(std::wstring_view title, std::wstring_view text) noexcept;     // tray balloon
    void setTimer(std::uint32_t id, std::chrono::milliseconds delay) noexcept; // one shot
    void killTimer(std::uint32_t id) noexcept;
    // Signals FolderChanged when a file in `folder` (not its subfolders) is created, renamed or written.
    [[nodiscard]] Result<void> watchFolder(std::uint32_t id, const std::filesystem::path& folder);

    // Pumps messages (and folder watches) until quit(). Returns the exit code passed to quit().
    int run();

    // Any thread.
    void post(std::function<void()> task); // runs `task` on the window thread
    void quit(int exitCode = 0) noexcept;

private:
    friend struct MessageWindowAccess; // the window procedure, in the .cpp

    MessageWindow() = default;
    std::intptr_t handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);
    void emit(const SystemEvent& event);
    void showMenu();
    void addTrayIcon() noexcept;

    void* m_window = nullptr; // HWND
    Handler m_handler;
    unsigned m_taskbarCreated = 0;
    std::uint32_t m_explorerPid = 0;
    std::vector<std::uint32_t> m_hotkeys;
    void* m_powerNotification = nullptr;  // HPOWERNOTIFY
    void* m_deviceNotification = nullptr; // HDEVNOTIFY
    bool m_sessionNotification = false;
    bool m_trayShown = false;
    std::wstring m_tooltip;
    std::vector<TrayItem> m_menu;
    void* m_icon = nullptr; // HICON (shared, not destroyed)
    struct Watch
    {
        std::uint32_t id = 0;
        void* handle = nullptr; // FindFirstChangeNotification
    };
    std::vector<Watch> m_watches;

    std::mutex m_tasksMutex;
    std::vector<std::function<void()>> m_tasks;
};

} // namespace rmw::platform
