#pragma once

#include "CoreServer.hpp"

#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/Settings.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/MessageWindow.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rmw::core
{

class WorkQueue;

struct LoadedSettings
{
    domain::Settings settings;
    domain::Diagnostics diagnostics;
    bool present = false; // the file exists
    bool valid = true;
    std::string error; // why it could not be used
};

[[nodiscard]] std::filesystem::path settingsFile(const std::filesystem::path& dataDirectory);
// Reads <data>\config\settings.toml. No file is a valid, empty configuration.
[[nodiscard]] LoadedSettings loadSettings(const std::filesystem::path& dataDirectory);

// The resident part of the core (docs/03 §2.2, §5.2, §9): hotkeys, rules, the appearance
// schedule, the tray and the reactions to system events (drift, Explorer restarts, monitors).
// Lives on the window thread and never blocks it: engine work goes to the work queue.
class Resident
{
public:
    Resident(CoreServer& core, platform::MessageWindow& window, WorkQueue& work);
    ~Resident() = default;
    Resident(const Resident&) = delete;
    Resident& operator=(const Resident&) = delete;
    Resident(Resident&&) = delete;
    Resident& operator=(Resident&&) = delete;

    // Window thread.
    void start();
    void onEvent(const platform::SystemEvent& event);
    // The schedule or the settings changed: residency, tray and timers follow.
    void refresh();
    [[nodiscard]] ipc::Json reloadSettings();
    // The supervisor gave up on rmw-bar (it failed too often).
    void onBarDisabled();

    // Any thread.
    [[nodiscard]] ipc::Json status() const;

private:
    [[nodiscard]] bool safe() const noexcept { return m_core->safeMode() != SafeModeReason::None; }
    void onTimer(std::uint32_t id);
    void onTray(std::uint32_t id);
    void onHotkey(std::uint32_t id);
    void onChanged(const std::string& module);
    void checkDrift();
    void onDisplaySettled();
    void onMonitors(std::vector<std::string> keys);
    void onPowerSource(bool onAc);
    void onClockJump();
    void onScheduleTimer();
    [[nodiscard]] bool due(const std::string& key, domain::TimeOfDay at, std::int32_t now);
    void armSchedule();
    void syncPages();
    void syncBar();
    void emergency();
    void armPages();
    void onPagesTimer();
    void applySettings(LoadedSettings loaded);
    void queueAction(domain::ActionCall action, Origin origin);
    void queueScheduleSync();
    void showTray();
    void notify(std::wstring_view title, const std::wstring& text);
    void publishStatus();

    CoreServer* m_core;
    platform::MessageWindow* m_window;
    WorkQueue* m_work;

    LoadedSettings m_loaded;    // what is in effect
    ipc::Json m_settingsReport; // the last read of settings.toml
    std::optional<std::filesystem::file_time_type> m_settingsStamp;
    std::vector<std::string> m_hotkeyErrors; // per hotkey; empty = registered
    bool m_resident = false;

    std::optional<bool> m_onAc;
    bool m_monitorsKnown = false;
    std::vector<std::string> m_monitors;
    std::vector<std::string> m_dirty; // modules with change events waiting for the debounce
    std::chrono::steady_clock::time_point m_displaySettlingUntil;
    std::chrono::steady_clock::time_point m_started;
    std::chrono::system_clock::time_point m_lastWallClock;
    std::map<std::string, std::chrono::steady_clock::time_point> m_lastFired;
    struct Pages
    {
        std::chrono::steady_clock::time_point due;
        std::chrono::milliseconds interval{0};
    };
    std::map<std::string, Pages> m_pages;                 // slideshows the core turns, per monitor field
    std::optional<std::string> m_barStartedFor;           // the plan rmw-bar was started with
    std::optional<std::filesystem::path> m_watchedSource; // the rice folder whose saves update the bars
    bool m_emergencyKey = false;

    mutable std::mutex m_statusMutex;
    ipc::Json m_status;
};

} // namespace rmw::core
