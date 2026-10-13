#pragma once

#include "WindowOps.hpp"

#include "rmw/tiling/Plan.hpp"

#include <cstdint>
#include <optional>
#include <set>

namespace rmw::tilerapp
{

// The rice's [windows] (M14, ADR-0021): Windows 11's DWM attributes — corners, border, title bar and
// title colors, backdrop, dark title bar — set on other apps' windows from the outside
// (DwmSetWindowAttribute talks to DWM, never to the app: nothing goes into their processes, and a
// hung app does not stop us). Every window it painted gets Windows' defaults back when the look
// changes or ends.
class Looks
{
public:
    Looks() = default;
    ~Looks();
    Looks(const Looks&) = delete;
    Looks& operator=(const Looks&) = delete;
    Looks(Looks&&) = delete;
    Looks& operator=(Looks&&) = delete;

    // A new look (or none): the old one comes off every window, the new one goes on every app window
    // there is.
    void set(const std::optional<tiling::WindowLook>& look);
    // A window appeared: it gets the look, unless its program is left out. Once per window, unless
    // `again` (something gave it Windows' attributes back). True when the list of painted windows grew.
    bool paint(Window window, bool again = false);
    bool forget(Window window) noexcept; // true when it was painted
    // Every window it painted back to Windows' own.
    void reset() noexcept;
    // What it painted, for the file a crash leaves behind (windows.json); nullopt while no look is on.
    [[nodiscard]] std::optional<tiling::Painted> painted() const;

    // The border a window keeps when the tiler's focus border leaves it: the look's, or Windows'.
    [[nodiscard]] std::optional<std::uint32_t> border() const noexcept;
    [[nodiscard]] bool active() const noexcept { return m_look.has_value(); }

private:
    std::optional<tiling::WindowLook> m_look;
    bool m_systemDark = false; // apps' dark mode when the look started: what dark title bars go back to
    std::set<Window> m_painted;
};

// Windows' own attributes back on `window`, for those `look` set (`systemDark`: apps' dark mode when it
// went on). Looks::reset, and `rmw-tiler --restore` for a tiler that ended before it could.
void unpaint(Window window, const tiling::WindowLook& look, bool systemDark) noexcept;

} // namespace rmw::tilerapp
