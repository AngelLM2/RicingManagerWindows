#pragma once

#include "rmw/domain/Settings.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Tiling (M13): where windows go, without Windows. Every layout is a pure function of the area, how
// many windows there are and the options, so it is tested to the pixel.
namespace rmw::tiling
{

struct Rect
{
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    [[nodiscard]] std::int32_t width() const noexcept { return right - left; }
    [[nodiscard]] std::int32_t height() const noexcept { return bottom - top; }
    [[nodiscard]] bool empty() const noexcept { return width() <= 0 || height() <= 0; }
    [[nodiscard]] bool contains(std::int32_t x, std::int32_t y) const noexcept
    {
        return x >= left && x < right && y >= top && y < bottom;
    }
    friend bool operator==(const Rect&, const Rect&) = default;
};

using LayoutKind = domain::TilingLayout;

// The next one, for "change the layout": Dwindle, MasterStack, Columns, Monocle, Dwindle again.
// Floating is chosen in the settings, never by the shortcut (the windows would stop being placed).
[[nodiscard]] LayoutKind nextLayout(LayoutKind layout) noexcept;

struct LayoutOptions
{
    std::int32_t outerGap = 8;  // between the windows and the area's edges (pixels)
    std::int32_t innerGap = 8;  // between two windows
    double masterRatio = 0.55;  // MasterStack: the master's share of the width
};

// Where each of `count` windows goes in `area`, in their order (the first is the oldest, the master
// of MasterStack). Floating: none (empty). Never smaller than 1x1, never outside the area.
[[nodiscard]] std::vector<Rect> arrange(LayoutKind layout, const Rect& area, std::size_t count,
                                        const LayoutOptions& options);

enum class Direction : std::uint8_t
{
    Left,
    Right,
    Up,
    Down,
};

// The window next to `from` in that direction: of those whose middle lies that way, the nearest
// (along the direction first, then across it). None at the edge.
[[nodiscard]] std::optional<std::size_t> neighbor(std::span<const Rect> rects, std::size_t from,
                                                  Direction direction) noexcept;

} // namespace rmw::tiling
