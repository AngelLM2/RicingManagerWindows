#include "rmw/tiling/Layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace rmw::tiling
{
namespace
{

// Dwindle stops halving below this (pixels): a window smaller than that is of no use.
constexpr std::int32_t kMinDwindleSlot = 64;

Rect inset(const Rect& rect, std::int32_t by) noexcept
{
    Rect out{rect.left + by, rect.top + by, rect.right - by, rect.bottom - by};
    if (out.width() < 1)
    {
        out.left = rect.left + (rect.width() / 2);
        out.right = out.left + 1;
    }
    if (out.height() < 1)
    {
        out.top = rect.top + (rect.height() / 2);
        out.bottom = out.top + 1;
    }
    return out;
}

// `rect` cut in two, side by side (`vertical`) or one above the other, `share` of it to the first
// part and a gap between them.
std::pair<Rect, Rect> split(const Rect& rect, bool vertical, double share, std::int32_t gap) noexcept
{
    Rect first = rect;
    Rect second = rect;
    if (vertical)
    {
        const std::int32_t room = std::max(rect.width() - gap, 2);
        const auto firstWidth = std::clamp(static_cast<std::int32_t>(std::lround(room * share)), 1, room - 1);
        first.right = rect.left + firstWidth;
        second.left = first.right + gap;
    }
    else
    {
        const std::int32_t room = std::max(rect.height() - gap, 2);
        const auto firstHeight = std::clamp(static_cast<std::int32_t>(std::lround(room * share)), 1, room - 1);
        first.bottom = rect.top + firstHeight;
        second.top = first.bottom + gap;
    }
    second.left = std::min(second.left, rect.right - 1);
    second.top = std::min(second.top, rect.bottom - 1);
    return {first, second};
}

// `rect` cut into `count` equal parts, side by side or one above the other. Too many for the room:
// the gaps shrink first; with less than a pixel each, the last ones share the edge.
std::vector<Rect> slices(const Rect& rect, std::size_t count, bool vertical, std::int32_t gap)
{
    std::vector<Rect> out;
    out.reserve(count);
    const auto n = static_cast<std::int32_t>(count);
    const std::int32_t length = vertical ? rect.width() : rect.height();
    if (n > 1)
    {
        gap = std::clamp((length - n) / (n - 1), 0, gap);
    }
    const std::int32_t room = std::max(length - (gap * (n - 1)), n);
    const std::int32_t end = vertical ? rect.right : rect.bottom;
    std::int32_t at = vertical ? rect.left : rect.top;
    for (std::int32_t i = 0; i < n; ++i)
    {
        // The rounding goes to the last ones: the parts add up to the room exactly.
        const std::int32_t size = ((room * (i + 1)) / n) - ((room * i) / n);
        Rect part = rect;
        const std::int32_t from = std::min(at, end - 1);
        const std::int32_t to = std::clamp(at + size, from + 1, end);
        if (vertical)
        {
            part.left = from;
            part.right = to;
        }
        else
        {
            part.top = from;
            part.bottom = to;
        }
        out.push_back(part);
        at += size + gap;
    }
    return out;
}

std::int64_t middleX(const Rect& r) noexcept
{
    return (static_cast<std::int64_t>(r.left) + r.right) / 2;
}

std::int64_t middleY(const Rect& r) noexcept
{
    return (static_cast<std::int64_t>(r.top) + r.bottom) / 2;
}

} // namespace

LayoutKind nextLayout(LayoutKind layout) noexcept
{
    switch (layout)
    {
    case LayoutKind::Dwindle:
        return LayoutKind::MasterStack;
    case LayoutKind::MasterStack:
        return LayoutKind::Columns;
    case LayoutKind::Columns:
        return LayoutKind::Monocle;
    case LayoutKind::Monocle:
    case LayoutKind::Floating:
        break;
    }
    return LayoutKind::Dwindle;
}

std::vector<Rect> arrange(LayoutKind layout, const Rect& area, std::size_t count, const LayoutOptions& options)
{
    // A window count beyond any screen's room: nobody tiles 4096 windows.
    if (count == 0 || count > 4096 || layout == LayoutKind::Floating || area.empty())
    {
        return {};
    }
    const Rect inner = inset(area, std::max(options.outerGap, 0));
    const std::int32_t gap = std::max(options.innerGap, 0);
    switch (layout)
    {
    case LayoutKind::Monocle:
        return std::vector<Rect>(count, inner);
    case LayoutKind::Columns:
        return slices(inner, count, true, gap);
    case LayoutKind::MasterStack: {
        if (count == 1)
        {
            return {inner};
        }
        const double ratio = std::isnan(options.masterRatio) ? 0.5 : std::clamp(options.masterRatio, 0.1, 0.9);
        const auto [master, stack] = split(inner, true, ratio, gap);
        std::vector<Rect> out{master};
        const auto rest = slices(stack, count - 1, false, gap);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    case LayoutKind::Dwindle: {
        std::vector<Rect> out;
        out.reserve(count);
        Rect rest = inner;
        while (out.size() + 1 < count)
        {
            const bool vertical = rest.width() >= rest.height();
            if ((vertical ? rest.width() : rest.height()) < (2 * kMinDwindleSlot) + gap)
            {
                break; // too small to halve again: the rest share it
            }
            const auto [taken, left] = split(rest, vertical, 0.5, gap);
            out.push_back(taken);
            rest = left;
        }
        if (out.size() + 1 == count)
        {
            out.push_back(rest);
            return out;
        }
        const auto shared = slices(rest, count - out.size(), rest.width() >= rest.height(), gap);
        out.insert(out.end(), shared.begin(), shared.end());
        return out;
    }
    case LayoutKind::Floating:
        break;
    }
    return {};
}

std::optional<std::size_t> neighbor(std::span<const Rect> rects, std::size_t from, Direction direction) noexcept
{
    if (from >= rects.size())
    {
        return std::nullopt;
    }
    const Rect& origin = rects[from];
    std::optional<std::size_t> best;
    std::int64_t bestAlong = std::numeric_limits<std::int64_t>::max();
    std::int64_t bestAcross = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < rects.size(); ++i)
    {
        if (i == from || rects[i] == origin) // Monocle: the same place is no neighbor
        {
            continue;
        }
        const Rect& other = rects[i];
        std::int64_t along = 0;
        std::int64_t across = 0;
        bool thatWay = false;
        switch (direction)
        {
        case Direction::Left:
            along = static_cast<std::int64_t>(origin.left) - other.right;
            across = std::abs(middleY(other) - middleY(origin));
            thatWay = middleX(other) < middleX(origin);
            break;
        case Direction::Right:
            along = static_cast<std::int64_t>(other.left) - origin.right;
            across = std::abs(middleY(other) - middleY(origin));
            thatWay = middleX(other) > middleX(origin);
            break;
        case Direction::Up:
            along = static_cast<std::int64_t>(origin.top) - other.bottom;
            across = std::abs(middleX(other) - middleX(origin));
            thatWay = middleY(other) < middleY(origin);
            break;
        case Direction::Down:
            along = static_cast<std::int64_t>(other.top) - origin.bottom;
            across = std::abs(middleX(other) - middleX(origin));
            thatWay = middleY(other) > middleY(origin);
            break;
        }
        if (!thatWay)
        {
            continue;
        }
        along = std::max<std::int64_t>(along, 0);
        if (along < bestAlong || (along == bestAlong && across < bestAcross))
        {
            best = i;
            bestAlong = along;
            bestAcross = across;
        }
    }
    return best;
}

} // namespace rmw::tiling
