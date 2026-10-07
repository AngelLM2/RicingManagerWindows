#pragma once

#include "Shell.hpp"
#include "Thumbnails.hpp"

#include "rmw/base/Error.hpp"
#include "rmw/client/CoreClient.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/settings/Model.hpp"
#include "rmw/settings/WallpaperEngine.hpp"

#include <app.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
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
    // Ricing Manager exits (the tray's Exit, `rmw shutdown`): from any thread, nothing starts the core
    // again; then, on the UI thread, the window closes.
    void closeForExit() noexcept { m_closing = true; }
    void hideForExit();
    // This window is replacing a core of another build (after an update): that core closes the settings
    // window as it stops, and this one stays.
    [[nodiscard]] bool replacingCore() const noexcept { return m_replacingCore.load(); }
    // The whole window is drawn again (it came back from off the screen, from minimized, from
    // another monitor): the renderer redraws only what it knows changed.
    void repaintAll();
    // The window's width in logical pixels: below 1000 the side bar shows icons only.
    void setWidth(double logical);
    // The window closed: the worker stops after what it does now. False when that takes longer than
    // `patience` (a call the core takes minutes over, a large copy): the process then leaves without it.
    [[nodiscard]] bool stop(std::chrono::milliseconds patience);

private:
    // Watches the window's messages for when it must be drawn whole again.
    void watchWindow();
    using Reply = Result<ipc::Json>;
    using Done = std::function<void(Reply)>;
    using Task = std::function<void(client::CoreClient&, const std::stop_token&)>;

    // ---- the core, on the worker thread --------------------------------------------------------
    // `operation`: a change (apply, restore...): waits while the core is busy, shows the busy state.
    void call(std::string method, ipc::Json params, Done done, bool operation = false);
    // Work of our own (no core, no window) on the same thread; `busy` shows the busy state.
    void background(std::function<Reply()> job, Done done, bool busy = true);
    void work(const std::stop_token& stop);
    // Waits `time` on the worker thread: true when the window closed meanwhile.
    bool pause(const std::stop_token& stop, std::chrono::milliseconds time);

    // ---- what the pages show ---------------------------------------------------------------------
    void bind();
    void bindTiling(); // the tiling page (M13)
    // A change on the tiling page: saved when it would read back (else the page says why).
    bool saveTiling(domain::Settings next);
    void refreshStatus();
    void refreshRices(); // reads every rice.toml: on the worker thread
    void showRices(const std::vector<std::filesystem::path>& remembered);
    void refreshPlugins();
    void refreshMonitors();
    void refreshSettings();
    void refreshDiagnostics();
    // The hotkeys of settings.toml, with what the core said of each in its last status (`force`:
    // even when that did not change).
    void showHotkeys(bool force = false);
    void notice(NoticeKind kind, const std::string& detail = {}, bool canUndo = false);
    void failed(const Error& error);
    // Something the window does not do, and why, in the user's language (`why`: a Words.problem key).
    void refused(std::string_view why);
    void applyLanguage();

    // ---- the rice being edited -------------------------------------------------------------------
    // On an editor page with nothing being edited: the applied rice, or the current look saved as
    // "My look".
    void ensureEditing(Page page);
    // `page`: where the window goes (none: it stays where the user is).
    void startEditing(const std::filesystem::path& folder, std::optional<Page> page);
    void showEdited();
    void showWallpapers();
    void showBars();
    // Writes rice.toml; `show` refreshes the pages from the model afterwards. A change that would
    // make the rice unreadable is not written: the model goes back to the file, the user hears why.
    void saveEdited(bool show = true);
    // The changes saved a moment after the last one (sliders, the bar's fields) are saved now:
    // before applying, sharing, editing another rice, closing.
    void flushEdits();
    void applyFolder(const std::filesystem::path& folder);
    [[nodiscard]] domain::Rice* edited() noexcept { return m_edit ? &m_edit->loaded.rice : nullptr; }
    // Copies a picked file or folder into the edited rice on the worker thread (a video may be 4 GB, a
    // slideshow 500 images, each hashed); then `use` gets its package path, if that rice is still the
    // one edited.
    void importInto(std::filesystem::path source, settings::AssetKind kind,
                    std::function<void(const std::string&)> use);
    [[nodiscard]] bool stillEditing(const std::filesystem::path& folder) const;

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
    // The selected plugin's entry in m_plugins (by its id: the list's order changes), or null.
    [[nodiscard]] const ipc::Json* selectedPlugin() const;
    void rememberFolder(const std::filesystem::path& folder);
    void forgetFolder(const std::filesystem::path& folder);
    void writeRemembered(const std::vector<std::filesystem::path>& folders);
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
    bool m_engineScanning = false;
    ipc::Json m_apps = ipc::Json::array();       // apps.list, once asked
    std::optional<std::pair<int, int>> m_appFor; // the hotkey (0) or rule (1) picking an app
    std::string m_selectedPlugin;                // the plugin whose settings show (its id)
    std::map<std::string, std::string> m_hotkeyProblems; // what the hotkey rows show, by keys
    std::vector<settings::RiceEntry> m_riceEntries;      // the library as last read
    int m_operations = 0;                        // running changes (the busy state)
    int m_appliedLanguage = -1;                  // what applyLanguage() last chose
    // One status, one library scan and one diagnostics read at a time: a slow core (or disk) would
    // get one every 3 s, and they would pile up behind it. "Again": one more once it answers.
    bool m_statusPending = false;
    bool m_statusAgain = false;
    bool m_ricesPending = false;
    bool m_ricesAgain = false;
    bool m_diagnosticsPending = false;
    slint::Timer m_refresh;
    slint::Timer m_windowSetup; // until the native window exists
    slint::Timer m_barSave; // the bar's settings are saved a moment after the last change

    std::atomic<bool> m_closing{false}; // Ricing Manager exits: the core is not started again
    std::atomic<bool> m_replacingCore{false};

    // The worker thread and what it is asked.
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_stopped;
    bool m_workerDone = false;
    std::deque<Task> m_tasks;
    bool m_repaintQueued = false;
    std::jthread m_worker; // last: stops first
};

} // namespace rmw::settingsapp
