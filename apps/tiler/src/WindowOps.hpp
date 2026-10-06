#pragma once

#include "rmw/tiling/Desktop.hpp"
#include "rmw/tiling/Plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

// What rmw-tiler does to other apps' windows, all from the outside (nothing is injected): it reads
// their styles, moves them with SetWindowPos, hides and shows them, and colors their DWM border.
// Every call that reaches another process is asynchronous, so a hung app never stops the tiler.
namespace rmw::tilerapp
{

using Window = void*; // HWND

[[nodiscard]] inline tiling::WindowId idOf(Window window) noexcept
{
    return reinterpret_cast<tiling::WindowId>(window);
}
[[nodiscard]] inline Window windowOf(tiling::WindowId id) noexcept
{
    return reinterpret_cast<Window>(static_cast<std::uintptr_t>(id)); // NOLINT(performance-no-int-to-ptr)
}

// An app's own top-level window as a person sees it: visible, not minimized, not cloaked, not a tool
// or a popup, no owner (dialogs stay with their window), with a frame, and not the shell's.
[[nodiscard]] bool isAppWindow(Window window) noexcept;
// It can be resized (a fixed-size window floats).
[[nodiscard]] bool isResizable(Window window) noexcept;
[[nodiscard]] tiling::WindowFacts factsOf(Window window);
[[nodiscard]] std::uint32_t processOf(Window window) noexcept;
// Its process runs at our integrity level or below (Windows lets us move it). An admin app's
// windows need the tiler to run as admin too.
[[nodiscard]] bool canMove(Window window) noexcept;
// This process runs at high integrity (as administrator): what Windows looks at to let a process move
// another's windows and hear the keys typed into them.
[[nodiscard]] bool runsAsAdmin() noexcept;
// One of Ricing Manager's own (the bars, the menu, the wallpaper); the settings window is an app.
[[nodiscard]] bool isOurs(Window window) noexcept;

// The frame a person sees (DWM's extended frame bounds, without the invisible resize borders).
[[nodiscard]] tiling::Rect frameOf(Window window) noexcept;
// Moves and sizes `window` so its visible frame lands on `frame`.
void place(Window window, const tiling::Rect& frame) noexcept;
// The invisible resize borders around the visible frame (outer minus visible, per side), read while the
// window shows: a hidden window has no visible frame to measure.
struct Insets
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};
[[nodiscard]] Insets insetsOf(Window window) noexcept;
// A hidden window shown in its slot (with the borders it had), above the others and without the
// focus: a workspace switch covers the old windows first, so no window shows anywhere but in its place.
void showInPlace(Window window, const tiling::Rect& frame, const Insets& insets) noexcept;
void hide(Window window) noexcept;
void show(Window window) noexcept; // without taking the focus
// The colored border of Windows 11 (DWMWA_BORDER_COLOR); nullopt: Windows' own again.
void setBorder(Window window, std::optional<std::uint32_t> colorref) noexcept;
// To the front, with the keyboard focus. Sends an unassigned key first: a process that just sent
// input may set the foreground window.
void focus(Window window) noexcept;
[[nodiscard]] Window foreground() noexcept;

// The monitors now, the primary first.
[[nodiscard]] std::vector<tiling::Monitor> monitorsNow();
// The monitor `window` is on, or the one under the mouse when it is null: its index in `monitors`.
[[nodiscard]] std::size_t monitorIndex(std::span<const tiling::Monitor> monitors, Window window) noexcept;

// What `window` is now, for putting it back later.
[[nodiscard]] tiling::Original originalOf(Window window) noexcept;
// Every window where it was: shown again, at its place, maximized if it was; Windows' border.
// A window that is gone, or whose handle another process now owns, is left alone. Returns the ones
// this process may not touch (an admin app's, for a tiler that is not elevated).
std::vector<tiling::Original> putBack(std::span<const tiling::Original> windows);
// The same, from the file a tiler that ended badly left: removed, or left with only the windows that
// need an elevated tiler (advanced mode puts them back when it starts). False when there was none.
bool putBackFromFile(const std::filesystem::path& file);

// Marks the keys the tiler sends itself, so its hook lets them through.
inline constexpr std::uintptr_t kOurKeys = 0x524D5754; // "RMWT"

} // namespace rmw::tilerapp
