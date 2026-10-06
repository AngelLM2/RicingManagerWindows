#include "rmw/bar/Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace rmw::bar
{
namespace
{

std::int32_t px(double logical, double scale) noexcept
{
    return static_cast<std::int32_t>(std::lround(logical * scale));
}

} // namespace

bool isHorizontal(domain::BarPosition position) noexcept
{
    return position == domain::BarPosition::Top || position == domain::BarPosition::Bottom;
}

Rect reservation(const domain::BarConfig& bar, const Rect& monitor, double scale) noexcept
{
    const auto& margin = bar.margin; // top, right, bottom, left
    const double across = isHorizontal(bar.position) ? margin[0] + bar.thickness + margin[2]
                                                     : margin[3] + bar.thickness + margin[1];
    const std::int32_t depth = px(across, scale);
    Rect strip = monitor;
    switch (bar.position)
    {
    case domain::BarPosition::Top:
        strip.bottom = monitor.top + depth;
        break;
    case domain::BarPosition::Bottom:
        strip.top = monitor.bottom - depth;
        break;
    case domain::BarPosition::Left:
        strip.right = monitor.left + depth;
        break;
    case domain::BarPosition::Right:
        strip.left = monitor.right - depth;
        break;
    }
    return strip;
}

Rect windowRect(const domain::BarConfig& bar, const Rect& area, double scale, double content) noexcept
{
    const auto& margin = bar.margin; // top, right, bottom, left
    const bool horizontal = isHorizontal(bar.position);
    const std::int32_t thickness = std::max(1, px(bar.thickness, scale));

    // Along the edge: margins at both ends, then the length, centered in what is left.
    const std::int32_t before = px(horizontal ? margin[3] : margin[0], scale);
    const std::int32_t after = px(horizontal ? margin[1] : margin[2], scale);
    const std::int32_t edge = horizontal ? area.width() : area.height();
    const std::int32_t available = std::max(0, edge - before - after);
    const std::int32_t wanted = bar.length.fit       ? std::max(px(content, scale), thickness)
                                : bar.length.percent ? static_cast<std::int32_t>(std::lround(edge * bar.length.value / 100.0))
                                                     : px(bar.length.value, scale);
    const std::int32_t length = std::clamp(wanted, 0, available);
    const std::int32_t start = (horizontal ? area.left : area.top) + before + ((available - length) / 2);

    Rect window;
    if (horizontal)
    {
        window.left = start;
        window.right = start + length;
        window.top = bar.position == domain::BarPosition::Top
                         ? area.top + px(margin[0], scale)
                         : area.bottom - px(margin[2], scale) - thickness;
        window.bottom = window.top + thickness;
    }
    else
    {
        window.top = start;
        window.bottom = start + length;
        window.left = bar.position == domain::BarPosition::Left
                          ? area.left + px(margin[3], scale)
                          : area.right - px(margin[1], scale) - thickness;
        window.right = window.left + thickness;
    }
    return window;
}

} // namespace rmw::bar
