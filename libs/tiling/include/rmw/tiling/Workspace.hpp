#pragma once

#include "rmw/tiling/Layout.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rmw::tiling
{

using WindowId = std::uint64_t; // an HWND, as a number

// The windows of one workspace and how they are laid out (M13), without Windows: which ones, in
// what order, which has the focus, which float, the layout. The tiler tells it what happens and
// asks it where everything goes.
class Workspace
{
public:
    explicit Workspace(LayoutKind layout = LayoutKind::Dwindle) noexcept
        : m_layout(layout)
    {
    }

    // A new window: after the others (the newest), with the focus.
    void add(WindowId window, bool floating = false);
    // False when it was not here.
    bool remove(WindowId window);
    [[nodiscard]] bool contains(WindowId window) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_windows.empty(); }
    // Every window, the floating ones too, oldest first.
    [[nodiscard]] std::span<const WindowId> windows() const noexcept { return m_windows; }

    void focus(WindowId window) noexcept; // only one of ours
    [[nodiscard]] std::optional<WindowId> focused() const noexcept { return m_focused; }

    // A floating window is left where it is (a dialog, a picture-in-picture...).
    void toggleFloating(WindowId window);
    [[nodiscard]] bool floating(WindowId window) const noexcept;

    [[nodiscard]] LayoutKind layout() const noexcept { return m_monocle ? LayoutKind::Monocle : m_layout; }
    void setLayout(LayoutKind layout) noexcept;
    void cycleLayout() noexcept;
    // Monocle and back to what it was: "full screen" for the focused window.
    void toggleMonocle() noexcept { m_monocle = !m_monocle; }
    [[nodiscard]] bool monocle() const noexcept { return m_monocle; }

    // The tiled windows (not floating), in order.
    [[nodiscard]] std::vector<WindowId> tiled() const;
    // Where each tiled window goes in `area`.
    [[nodiscard]] std::vector<std::pair<WindowId, Rect>> placements(const Rect& area,
                                                                    const LayoutOptions& options) const;
    // The tiled window next to `window` that way, as they are placed now.
    [[nodiscard]] std::optional<WindowId> neighborOf(WindowId window, Direction direction, const Rect& area,
                                                     const LayoutOptions& options) const;
    // `window` and its neighbor that way trade places. False when there is none.
    bool exchange(WindowId window, Direction direction, const Rect& area, const LayoutOptions& options);
    // `a` and `b` trade places (a window dragged onto another).
    bool exchange(WindowId a, WindowId b);
    // `window` becomes the first (MasterStack's master).
    void promote(WindowId window);

private:
    // The minimums of `windows` from the options, in order (empty when none is known).
    [[nodiscard]] static std::vector<Size> minimumsOf(std::span<const WindowId> windows, const LayoutOptions& options);

    std::vector<WindowId> m_windows; // oldest first
    std::vector<WindowId> m_floating;
    std::optional<WindowId> m_focused;
    LayoutKind m_layout;
    bool m_monocle = false;
};

} // namespace rmw::tiling
