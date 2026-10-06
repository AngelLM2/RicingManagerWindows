#pragma once

#include "KeyHook.hpp"
#include "Looks.hpp"
#include "SharedBlock.hpp"
#include "VirtualDesktops.hpp"
#include "WindowOps.hpp"

#include "rmw/platform/Supervisor.hpp"
#include "rmw/tiling/Desktop.hpp"
#include "rmw/tiling/Plan.hpp"
#include "rmw/tiling/Shared.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace rmw::tilerapp
{

// rmw-tiler's window thread (M13): it follows the plan file, hears what windows do (WinEvents, out
// of context), keeps rmw::tiling's model of every monitor's workspaces and puts the windows where
// the model says. Whatever ends it — the plan gone, the core gone, the session ending, Toggle — every
// window goes back where it was; a crash leaves <data>\tiler\windows.json for whoever comes next.
class TilerApp
{
public:
    // Why run() ended.
    enum class Ending : std::uint8_t
    {
        Done,      // the plan went, the core went, the session ended: every window is back
        Failed,    // it could not even start
        BeAdmin,   // the plan asks for advanced mode: an elevated copy takes over
        BePlain,   // (the elevated copy) the plan no longer asks for it: the plain one takes over
    };

    struct Options
    {
        bool elevated = false;      // this process runs as administrator
        bool adminDeclined = false; // the person said no to the UAC prompt: no asking again for now
        bool elevatedCopy = false;  // started elevated by a plain tiler (advanced mode)
    };

    // `core`: what ends it — the core's process, or (the elevated copy) the plain tiler's — or null
    // when started by hand.
    TilerApp(std::filesystem::path planFile, platform::Heartbeat& heartbeat, void* core, Options options);
    ~TilerApp();
    TilerApp(const TilerApp&) = delete;
    TilerApp& operator=(const TilerApp&) = delete;
    TilerApp(TilerApp&&) = delete;
    TilerApp& operator=(TilerApp&&) = delete;

    // Until the plan goes away, the core ends, the session ends or advanced mode changes.
    [[nodiscard]] Ending run();

private:
    struct Access;

    [[nodiscard]] static void* makeControlWindow();
    std::intptr_t handle(std::uint32_t message, std::uintptr_t wParam, std::intptr_t lParam);
    void onEvent(std::uint32_t event, Window window);
    void onForeground(Window window);
    void onMoved(Window window);

    // False when there is no plan (anymore).
    bool readPlan();
    void applyPlan(tiling::Plan plan);
    [[nodiscard]] std::vector<domain::TilingKey> hookKeys() const;
    void start(); // every window there is joins
    void stop();  // every window back; nothing is ours
    void adopt(Window window);
    void forget(Window window, bool gone);
    void remember(tiling::WindowId id, Window window);
    void apply(tiling::Change change);
    void hideFollowers(tiling::WindowId owner);
    void showFollowers(tiling::WindowId owner);

    void scheduleLayout();
    void layout();
    void settle();
    // In Mac mode, whether `window` is on the virtual desktop shown now (focusing one on another desktop
    // makes Windows go there).
    [[nodiscard]] bool onShownDesktop(Window window) const;
    // The windows a workspace switch brings, shown in their slots and above the ones that go.
    void placeComing(std::span<const tiling::WindowId> coming);
    void command(domain::TilingCommand command, int number);
    void focusToward(tiling::Direction direction);
    void moveToward(tiling::Direction direction);
    void goTo(std::size_t monitor, int workspace);
    void syncMonitors();
    void syncDesktops(); // the Mac style: Windows' virtual desktops changed
    void updateBorder(Window focused);
    void scheduleSave();
    void save();
    void publish();
    [[nodiscard]] std::size_t currentMonitor() const;
    [[nodiscard]] tiling::LayoutOptions options(std::size_t monitor) const;
    [[nodiscard]] bool mac() const noexcept;

    std::filesystem::path m_planFile;
    std::filesystem::path m_windowsFile;
    platform::Heartbeat* m_heartbeat;
    void* m_core;
    Options m_options;
    Ending m_ending = Ending::Done;
    void* m_window = nullptr; // the control window (hidden, top-level: it hears the session end)
    SharedBlock m_shared;     // what the bars read
    void* m_planWatch = nullptr;
    std::string m_planText;
    std::optional<tiling::Plan> m_plan;
    std::unique_ptr<tiling::Desktop> m_desktop; // null while off (Toggle) or before the start
    std::vector<tiling::Monitor> m_monitors;
    std::map<tiling::WindowId, tiling::Original> m_originals; // what goes back
    std::set<tiling::WindowId> m_hidden;                      // hidden by us
    std::map<tiling::WindowId, std::vector<tiling::WindowId>> m_followers; // dialogs hidden with them
    std::map<tiling::WindowId, tiling::Rect> m_targets;                   // where the shown ones go
    std::map<tiling::WindowId, Insets> m_insets; // the hidden ones' borders, read as they hid
    std::set<tiling::WindowId> m_justShown;      // shown in place this moment: settle() looks at them
    std::optional<tiling::WindowId> m_bordered;
    std::optional<tiling::WindowId> m_pendingFocus;
    std::set<tiling::WindowId> m_appearing; // shown a moment ago: tiled if still there (kAdoptDelayMs)
    std::unique_ptr<KeyHook> m_keys;
    Looks m_looks; // the rice's [windows] (M14): with tiling or alone
    std::unique_ptr<VirtualDesktops> m_virtual;
    VirtualDesktops::State m_virtualState;
    std::vector<void*> m_hooks; // HWINEVENTHOOK
    bool m_paused = false;
    bool m_quit = false;
    int m_settleRounds = 0;
    std::uint64_t m_lastAreaCheck = 0; // GetTickCount64
    bool m_toldMacSend = false;
};

} // namespace rmw::tilerapp
