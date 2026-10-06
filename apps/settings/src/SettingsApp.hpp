#pragma once

#include "Shell.hpp"
#include "Thumbnails.hpp"

#include "rmw/base/Error.hpp"
#include "rmw/client/CoreClient.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/settings/Model.hpp"
#include "rmw/settings/WallpaperEngine.hpp"

#include <app.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rmw::settingsapp
{

// The settings window (docs/04 M7): fills the pages from the core, the rice being edited and
// settings.toml, and does what they ask. The window thread never waits on the core: calls go to a
// thread of their own and their answers come back to the window thread.
class SettingsApp
{
public:
    explicit SettingsApp(std::filesystem::path dataDirectory);
    ~SettingsApp();
    SettingsApp(const SettingsApp&) = delete;
    SettingsApp& operator=(const SettingsApp&) = delete;
    SettingsApp(SettingsApp&&) = delete;
    SettingsApp& operator=(SettingsApp&&) = delete;

    // Shows the window until it is closed.
    void run();
    // Another rmw-settings was started: this window comes to the front.
    void bringToFront();
    // The whole window is drawn again (it came back from off the screen, from minimized, from
    // another monitor): the renderer redraws only what it knows changed.
    void repaintAll();

private:
    // Watches the window's messages for when it must be drawn whole again.
    void watchWindow();
    using Reply = Result<ipc::Json>;
    using Done = std::function<void(Reply)>;

    // ---- the core, on the worker thread --------------------------------------------------------
    // `operation`: a change (apply, restore...): waits while the core is busy, shows the busy state.
    void call(std::string method, ipc::Json params, Done done, bool operation = false);
    // Work of our own (no core) on the same thread, shown as busy.
    void background(std::function<Reply()> job, Done done);
    void work(const std::stop_token& stop);

    // ---- what the pages show ---------------------------------------------------------------------
    void bind();
    void bindTiling(); // the tiling page (M13)
    // A change on the tiling page: saved when it would read back (else the page says why).
    bool saveTiling(domain::Settings next);
    void refreshStatus();
    void refreshRices();
    void refreshPlugins();
    void refreshMonitors();
    void refreshSettings();
    void refreshDiagnostics();
    void notice(NoticeKind kind, const std::string& detail = {}, bool canUndo = false);
    void failed(const Error& error);
    void applyLanguage();

    // ---- the rice being edited -------------------------------------------------------------------
    // On an editor page with nothing being edited: the applied rice, or the current look saved as
    // "My look".
    void ensureEditing(Page page);
    void startEditing(const std::filesystem::path& folder, Page page);
    void showEdited();
    void showWallpapers();
    void showBars();
    // Writes rice.toml; `show` refreshes the pages from the model afterwards.
    void saveEdited(bool show = true);
    void applyFolder(const std::filesystem::path& folder);
    [[nodiscard]] domain::Rice* edited() noexcept { return m_edit ? &m_edit->loaded.rice : nullptr; }

    // ---- helpers ---------------------------------------------------------------------------------
    void* window() const;
    void fitToMonitor();
    [[nodiscard]] std::vector<std::string> knownActions() const;
    void saveSettings();
    void showApps(std::string_view filter);
    [[nodiscard]] std::string appName(std::string_view id) const;
    void matchChoices();
    void widgetChoices();
    void pluginConfig();
    [[nodiscard]] std::string pluginName(std::string_view id) const;
    void rememberFolder(const std::filesystem::path& folder);
    void engineScan(); // looks for Wallpaper Engine's wallpapers, then lists them
    [[nodiscard]] std::vector<std::filesystem::path> rememberedFolders() const;

    struct Edit
    {
        std::filesystem::path folder;
        settings::LoadedRice loaded;
    };

    std::filesystem::path m_dataDirectory;
    slint::ComponentHandle<AppWindow> m_ui;
    Thumbnails m_thumbnails;
    ProcessWatch m_processes;
    std::optional<Edit> m_edit;
    bool m_preparingEdit = false;
    std::optional<domain::Settings> m_settings;
    ipc::Json m_status;
    ipc::Json m_plugins = ipc::Json::array();
    ipc::Json m_monitors = ipc::Json::array();
    std::vector<std::string> m_actionNames;
    std::vector<std::string> m_matchChoices;
    // Wallpaper Engine (M12): what was found, the folders the user pointed at, the rule it is for.
    std::vector<settings::EngineWallpaper> m_engine;
    std::vector<std::filesystem::path> m_engineFolders;
    int m_engineRule = -1;
    ipc::Json m_apps = ipc::Json::array();       // apps.list, once asked
    std::optional<std::pair<int, int>> m_appFor; // the hotkey (0) or rule (1) picking an app
    int m_operations = 0;                        // running changes (the busy state)
    int m_appliedLanguage = -1;                  // what applyLanguage() last chose
    slint::Timer m_refresh;
    slint::Timer m_barSave; // the bar's settings are saved a moment after the last change

    // The worker thread and what it is asked.
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::function<void(client::CoreClient&)>> m_tasks;
    bool m_repaintQueued = false;
    std::jthread m_worker; // last: stops first
};

} // namespace rmw::settingsapp
