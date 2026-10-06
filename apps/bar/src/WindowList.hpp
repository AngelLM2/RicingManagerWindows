#pragma once

#include "Widgets.hpp"

#include "rmw/providers/Windows.hpp"

#include <slint.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rmw::barapp
{

// The windows a taskbar shows (the Alt+Tab rules: visible, unowned or app windows, not tool
// windows, not cloaked — so not those of other virtual desktops), kept current by WinEvent hooks.
// The hooks are out of context: nothing of ours goes into other processes. One per process.
class WindowTracker
{
public:
    struct Entry
    {
        void* window = nullptr; // HWND
        std::string title;
        slint::Image icon;
        bool iconLoaded = false;
    };

    static WindowTracker& instance();
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;
    WindowTracker(WindowTracker&&) = delete;
    WindowTracker& operator=(WindowTracker&&) = delete;

    [[nodiscard]] std::size_t subscribe(std::function<void()> changed);
    void unsubscribe(std::size_t id);

    // Oldest first, like the taskbar.
    [[nodiscard]] const std::vector<Entry>& windows() const noexcept { return m_windows; }
    [[nodiscard]] void* foreground() const noexcept { return m_foreground; }

    // Brings a window forward, or minimizes it when it already is in front (what a taskbar
    // button does).
    static void activate(void* window);

private:
    struct Access;
    WindowTracker();
    void schedule();
    void refresh();
    // The app's icon from the Shell (sharp at any scale), once per app; the window's own otherwise.
    slint::Image iconFor(void* window);

    std::unique_ptr<providers::WindowEventHooks> m_hooks;
    std::vector<Entry> m_windows;
    std::map<std::wstring, slint::Image> m_appIcons;
    void* m_foreground = nullptr;
    std::map<std::size_t, std::function<void()>> m_subscribers;
    std::size_t m_nextSubscriber = 1;
    bool m_scheduled = false;
};

// The "windows" widget: one button per window (icon, and the title unless labels = false).
[[nodiscard]] std::unique_ptr<Provider> makeWindowListProvider(const bar::Slot& slot,
                                                               const Instance& instance);

} // namespace rmw::barapp
