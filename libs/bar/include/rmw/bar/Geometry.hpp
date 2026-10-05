#pragma once

#include "rmw/domain/Rice.hpp"

#include <cstdint>

// Where a bar goes on its monitor, in physical pixels. `scale` is the monitor's DPI / 96: every
// size in the rice is in logical pixels.
namespace rmw::bar
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

    friend bool operator==(const Rect&, const Rect&) = default;
};

[[nodiscard]] bool isHorizontal(domain::BarPosition position) noexcept;

// What a Reserve bar asks the system for (an app bar): the whole strip along its edge, its
// thickness plus the margins before and after it.
[[nodiscard]] Rect reservation(const domain::BarConfig& bar, const Rect& monitor, double scale) noexcept;

// The bar window inside `area` — the strip the system granted (Reserve), or the monitor's work
// area (Float): margins applied, length along the edge (percent or pixels) centered.
[[nodiscard]] Rect windowRect(const domain::BarConfig& bar, const Rect& area, double scale) noexcept;

} // namespace rmw::bar
