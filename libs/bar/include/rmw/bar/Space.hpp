#pragma once

#include "rmw/bar/Geometry.hpp"
#include "rmw/base/Error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What rmw-bar holds of the screen, kept in <data>\bar\space.json while it runs: its app bar windows
// and the strips it set aside. A bar that leaves gives them back and removes the file; one that crashed,
// hung or was ended cannot, and the core gives them back from the file (an empty reserved strip after
// an exit, an update or an uninstall was the lifecycle review's first finding, v1.8.1).
namespace rmw::bar
{

struct SpaceEntry
{
    std::uint64_t window = 0; // HWND of the bar: an app bar (ABM_NEW) until the shell is told otherwise
    Rect monitor;             // the monitor's full rectangle
    Rect reserved;            // the strip the shell granted it (Reserve); empty: none
    bool claimed = false;     // the bar took the strip out of the work area itself (SPI_SETWORKAREA)

    friend bool operator==(const SpaceEntry&, const SpaceEntry&) = default;
};

[[nodiscard]] std::string spaceToJson(std::span<const SpaceEntry> entries);
// Checked like any input: the core reads what another process wrote.
[[nodiscard]] Result<std::vector<SpaceEntry>> parseSpace(std::string_view text);

// The work area to announce for `entry`'s monitor once its strip is free: `work` (the area Windows
// counts now) grown over the strip, as the bar does itself when it leaves. Announced, it makes Explorer
// work the area out again its own way (Windows' taskbar and the other app bars stay out of it).
[[nodiscard]] Rect workAreaWithout(const SpaceEntry& entry, const Rect& work) noexcept;

} // namespace rmw::bar
