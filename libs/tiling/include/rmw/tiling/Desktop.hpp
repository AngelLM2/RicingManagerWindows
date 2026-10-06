#pragma once

#include "rmw/tiling/Workspace.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rmw::tiling
{

struct Monitor
{
    std::string key; // Windows' name for it ("\\.\DISPLAY1"): the same while it stays connected
    Rect work;       // its work area (what the bars reserve left out)
    Rect bounds;     // the whole monitor (a bar finds its monitor by it)
    double scale = 1.0;

    friend bool operator==(const Monitor&, const Monitor&) = default;
};

// Where a window lives: a monitor (its index in monitors()) and one of its workspaces (from 0).
struct Place
{
    std::size_t monitor = 0;
    int workspace = 0;

    friend bool operator==(const Place&, const Place&) = default;
};

// What the tiler has to do after a change: the windows to hide (their workspace left the screen),
// the ones to show, and the one that gets the focus.
struct Change
{
    std::vector<WindowId> hide;
    std::vector<WindowId> show;
    std::optional<WindowId> focus;
};

// The workspaces of every monitor (M13), without Windows. Linux style: each monitor shows one of its
// own 1..N; Mac style: one per virtual desktop, the same on every monitor (Windows hides the others
// and the tiler only keeps track).
class Desktop
{
public:
    Desktop(int workspaces, LayoutKind layout);

    // The monitors now (the first is the primary). One that stays (same name, or same place under a new
    // name) keeps its workspaces; the workspaces of one that went away join the first's of the same
    // number, the one it showed joining the one shown. No monitors: nothing changes.
    Change setMonitors(std::vector<Monitor> monitors);
    [[nodiscard]] std::vector<Monitor> monitors() const;
    [[nodiscard]] std::size_t monitorCount() const noexcept { return m_screens.size(); }
    [[nodiscard]] int workspaceCount() const noexcept { return m_count; }
    // Fewer workspaces: the windows of the ones gone join the last one left.
    Change setWorkspaceCount(int count);
    // Every workspace takes this layout.
    void setLayout(LayoutKind layout);

    // A new window on `monitor`: in its active workspace, or in `workspace` (a rule; hidden when that
    // one is not shown). Nothing when it is already ours.
    Change add(WindowId window, std::size_t monitor, std::optional<int> workspace = std::nullopt,
               bool floating = false);
    // Gone (closed, minimized, hidden by its app). False when it was not ours.
    bool remove(WindowId window);
    [[nodiscard]] std::optional<Place> find(WindowId window) const noexcept;
    // In a workspace a monitor shows.
    [[nodiscard]] bool shown(WindowId window) const noexcept;

    [[nodiscard]] int active(std::size_t monitor) const noexcept;
    [[nodiscard]] Workspace& workspace(const Place& place);
    [[nodiscard]] const Workspace& workspace(const Place& place) const;
    [[nodiscard]] Workspace& activeWorkspace(std::size_t monitor);
    [[nodiscard]] const Rect& area(std::size_t monitor) const;
    // Bit i: workspace i has windows.
    [[nodiscard]] std::uint32_t occupied(std::size_t monitor) const noexcept;

    // Where every shown, tiled window goes.
    [[nodiscard]] std::vector<std::pair<WindowId, Rect>> placements(const LayoutOptions& options) const;

    // `monitor` shows workspace `workspace`.
    Change goTo(std::size_t monitor, int workspace);
    // `window` into workspace `workspace` of its monitor (hidden when that one is not shown).
    Change sendTo(WindowId window, int workspace);
    // `window` into the active workspace of `monitor`.
    Change moveToMonitor(WindowId window, std::size_t monitor);
    // The nearest monitor that way from `from`.
    [[nodiscard]] std::optional<std::size_t> monitorToward(std::size_t from, Direction direction) const;
    // Every window (to put them all back).
    [[nodiscard]] std::vector<WindowId> all() const;

private:
    struct Screen
    {
        Monitor monitor;
        std::vector<Workspace> workspaces;
        int active = 0;
    };

    [[nodiscard]] Screen makeScreen(Monitor monitor) const;
    [[nodiscard]] std::vector<WindowId> shownWindows() const;
    // What changed between two lists of shown windows.
    static Change difference(const std::vector<WindowId>& before, const std::vector<WindowId>& after);

    std::vector<Screen> m_screens;
    int m_count;
    LayoutKind m_layout;
};

} // namespace rmw::tiling
