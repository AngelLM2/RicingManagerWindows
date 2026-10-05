#pragma once

#include "BarWindow.hpp"
#include "SlintPlatform.hpp"
#include "Widgets.hpp"

#include "rmw/platform/NativeTaskbar.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rmw::barapp
{

// The bars of the plan, one window per matching monitor. Rebuilt when the plan changes (the core
// rewrote it), when monitors come and go, and when Explorer restarts (app bars and the hidden
// taskbar belong to it). With nothing to show it ends the loop: the core shows the native
// taskbar again when we exit.
class BarApp
{
public:
    BarApp(Win32Platform& loop, std::filesystem::path planFile);
    ~BarApp();
    BarApp(const BarApp&) = delete;
    BarApp& operator=(const BarApp&) = delete;
    BarApp(BarApp&&) = delete;
    BarApp& operator=(BarApp&&) = delete;

    // Shows the plan's bars and starts watching; false when there is no plan (or no bar in it).
    // A plan whose monitors are all absent keeps running: they may come back.
    [[nodiscard]] bool start();
    // The platform asks for the window of the bar being created.
    [[nodiscard]] std::unique_ptr<slint::platform::WindowAdapter> createAdapter();

private:
    struct Access;
    struct Shown
    {
        Instance instance;
        std::vector<std::unique_ptr<Provider>> providers;
    };
    struct Pending
    {
        domain::BarConfig config;
        void* monitor = nullptr;
    };

    [[nodiscard]] bool rebuild();
    void clear();
    void scheduleRebuild();

    Win32Platform* m_loop;
    std::filesystem::path m_planFile;
    std::vector<std::unique_ptr<Shown>> m_bars;
    std::optional<Pending> m_pending;
    BarWindow* m_created = nullptr; // the window Slint just took (it owns it)
    platform::NativeTaskbarHider m_native;
    void* m_events = nullptr;    // HWND: display changes, Explorer restarts, the rebuild timer
    void* m_planWatch = nullptr; // change notification on the plan's folder
    unsigned m_taskbarCreated = 0;
};

} // namespace rmw::barapp
