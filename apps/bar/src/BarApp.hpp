#pragma once

#include "BarWindow.hpp"
#include "PluginBridge.hpp"
#include "PluginWidgets.hpp"
#include "SlintPlatform.hpp"
#include "Widgets.hpp"

#include "rmw/platform/NativeTaskbar.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
    // `bridgePipe`: where the plugin host reaches us (empty: no plugin widgets get data).
    BarApp(Win32Platform& loop, std::filesystem::path planFile, std::wstring bridgePipe = {});
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
    // A plugin widget on screen: what the host addresses as `key` ("<bar>/<slot>").
    struct PluginSlot
    {
        std::string key;
        const PluginWidget* widget = nullptr;
        std::string prefix;
        slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> instance;
        domain::Value config;
    };

    [[nodiscard]] bool rebuild();
    void clear();
    void scheduleRebuild();
    void scheduleReload(); // a plugin being written changed: rebuild soon
    void watchDevPlugins(const std::vector<bar::PlanPlugin>& plugins);
    void onPluginMessage(bar::bridge::Message message);
    void sendInstances();
    void applyProperties(const PluginSlot& slot, const domain::Value::Table& properties);
    void windowAction(const bar::bridge::Action& action);

    Win32Platform* m_loop;
    std::filesystem::path m_planFile;
    std::vector<std::unique_ptr<Shown>> m_bars;
    std::optional<Pending> m_pending;
    BarWindow* m_created = nullptr; // the window Slint just took (it owns it)
    platform::NativeTaskbarHider m_native;
    void* m_events = nullptr;    // HWND: display changes, Explorer restarts, the rebuild timer
    void* m_planWatch = nullptr; // change notification on the plan's folder
    unsigned m_taskbarCreated = 0;

    PluginWidgets m_plugins;
    std::vector<PluginSlot> m_pluginSlots;
    // What the host last set on each widget (and for which plugin): shown again after a rebuild.
    struct CachedValues
    {
        std::string plugin;
        domain::Value::Table values;
    };
    std::map<std::string, CachedValues, std::less<>> m_pluginValues;
    std::vector<std::pair<std::filesystem::path, void*>> m_devWatches; // folder, change notification
    std::size_t m_windowSubscription = 0;
    std::unique_ptr<PluginBridge> m_bridge; // last: its callbacks reach everything above
};

} // namespace rmw::barapp
