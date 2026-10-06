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

// Below this, a window of unknown minimum counts as this big when minimums do not fit (pixels).
constexpr std::int32_t kUnknownMinimum = 64;
// When a cut makes room for a neighbour's minimum, a window keeps at least this (or a quarter of the room).
constexpr std::int32_t kUsefulSlot = 200;

Size minimumAt(std::span<const Size> minimums, std::size_t index) noexcept
{
    return index < minimums.size() ? Size{std::max(minimums[index].width, 0), std::max(minimums[index].height, 0)}
                                   : Size{};
}

// The largest minimum, width and height apart, of the windows from `first` to before `end`.
Size largestFrom(std::span<const Size> minimums, std::size_t first, std::size_t end) noexcept
{
    Size out;
    for (std::size_t i = first; i < end; ++i)
    {
        const Size size = minimumAt(minimums, i);
        out.width = std::max(out.width, size.width);
        out.height = std::max(out.height, size.height);
    }
    return out;
}

std::vector<std::int32_t> along(std::span<const Size> minimums, std::size_t first, std::size_t end, bool vertical)
{
    std::vector<std::int32_t> out;
    out.reserve(end > first ? end - first : 0);
    for (std::size_t i = first; i < end; ++i)
    {
        const Size size = minimumAt(minimums, i);
        out.push_back(vertical ? size.width : size.height);
    }
    return out;
}

// Lengths of `count` parts sharing `room`: equal, but none under its minimum when the room allows (the
// others share what is left); when the minimums do not fit, in proportion to them. They add up to `room`.
std::vector<std::int32_t> shares(std::int32_t room, std::size_t count, std::span<const std::int32_t> minimums)
{
    std::vector<std::int32_t> out(count, 0);
    if (count == 0)
    {
        return out;
    }
    const std::int64_t total = std::max(room, 0);
    const auto minimum = [&](std::size_t i) -> std::int64_t {
        return i < minimums.size() ? std::max(minimums[i], 0) : 0;
    };
    std::int64_t needed = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        needed += minimum(i);
    }
    if (needed > total)
    {
        std::int64_t weights = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            weights += std::max(minimum(i), std::int64_t{kUnknownMinimum});
        }
        std::int64_t before = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::int64_t upTo = before + std::max(minimum(i), std::int64_t{kUnknownMinimum});
            out[i] = static_cast<std::int32_t>(((total * upTo) / weights) - ((total * before) / weights));
            before = upTo;
        }
        return out;
    }
    std::vector<bool> held(count, false);
    std::int64_t left = total;
    auto open = static_cast<std::int64_t>(count);
    for (bool changed = true; changed && open > 0;)
    {
        changed = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!held[i] && minimum(i) * open > left)
            {
                held[i] = true;
                out[i] = static_cast<std::int32_t>(minimum(i));
                left -= minimum(i);
                --open;
                changed = true;
            }
        }
    }
    std::int64_t k = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!held[i])
        {
            out[i] = static_cast<std::int32_t>(((left * (k + 1)) / open) - ((left * k) / open));
            ++k;
        }
    }
    return out;
}

// The first part's length when `room` is cut in two: `share` of it, moved so each part keeps its
// minimum when both fit, in proportion to the minimums when they do not. A part with no minimum known
// still keeps a useful width (never 60 px so that a neighbour gets its minimum).
std::int32_t firstPart(std::int32_t room, double share, std::int32_t needFirst, std::int32_t needSecond) noexcept
{
    const std::int32_t useful = std::min(kUsefulSlot, room / 4);
    needFirst = std::max(needFirst, useful);
    needSecond = std::max(needSecond, useful);
    auto first = static_cast<std::int32_t>(std::lround(room * share));
    const std::int64_t needed = static_cast<std::int64_t>(needFirst) + needSecond;
    if (needed <= room)
    {
        first = std::clamp(first, needFirst, room - needSecond);
    }
    else
    {
        first = static_cast<std::int32_t>((static_cast<std::int64_t>(room) * needFirst) / needed);
    }
    return std::clamp(first, 1, std::max(room - 1, 1));
}

// `rect` cut in two, side by side (`vertical`) or one above the other, `first` pixels to the first
// part and a gap between them.
std::pair<Rect, Rect> splitAt(const Rect& rect, bool vertical, std::int32_t first, std::int32_t gap) noexcept
{
    Rect one = rect;
    Rect two = rect;
    if (vertical)
    {
        const std::int32_t room = std::max(rect.width() - gap, 2);
        one.right = rect.left + std::clamp(first, 1, room - 1);
        two.left = one.right + gap;
    }
    else
    {
        const std::int32_t room = std::max(rect.height() - gap, 2);
        one.bottom = rect.top + std::clamp(first, 1, room - 1);
        two.top = one.bottom + gap;
    }
    two.left = std::min(two.left, rect.right - 1);
    two.top = std::min(two.top, rect.bottom - 1);
    return {one, two};
}

// `rect` cut into `count` parts, side by side or one above the other: equal, or as `minimums` (along
// the cut) need. Too many for the room: the gaps shrink first; with less than a pixel each, the last
// ones share the edge.
std::vector<Rect> slices(const Rect& rect, std::size_t count, bool vertical, std::int32_t gap,
                         std::span<const std::int32_t> minimums = {})
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
    // The rounding goes to the last ones: the parts add up to the room exactly.
    const auto lengths = shares(room, count, minimums);
    for (std::int32_t i = 0; i < n; ++i)
    {
        const std::int32_t size = lengths[static_cast<std::size_t>(i)];
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

std::vector<Rect> arrange(LayoutKind layout, const Rect& area, std::size_t count, const LayoutOptions& options,
                          std::span<const Size> minimums)
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
        return slices(inner, count, true, gap, along(minimums, 0, count, true));
    case LayoutKind::MasterStack: {
        if (count == 1)
        {
            return {inner};
        }
        const double ratio = std::isnan(options.masterRatio) ? 0.5 : std::clamp(options.masterRatio, 0.1, 0.9);
        const std::int32_t room = std::max(inner.width() - gap, 2);
        const std::int32_t first =
            firstPart(room, ratio, minimumAt(minimums, 0).width, largestFrom(minimums, 1, count).width);
        const auto [master, stack] = splitAt(inner, true, first, gap);
        std::vector<Rect> out{master};
        const auto rest = slices(stack, count - 1, false, gap, along(minimums, 1, count, false));
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    case LayoutKind::Dwindle: {
        std::vector<Rect> out;
        out.reserve(count);
        Rect rest = inner;
        while (out.size() + 1 < count)
        {
            const std::size_t index = out.size();
            const Size taken = minimumAt(minimums, index);
            const Size after = largestFrom(minimums, index + 1, count);
            // Cut the long way, unless only the other way keeps every minimum.
            const auto fits = [&](bool sideBySide) {
                return sideBySide ? taken.width + gap + after.width <= rest.width() &&
                                        std::max(taken.height, after.height) <= rest.height()
                                  : taken.height + gap + after.height <= rest.height() &&
                                        std::max(taken.width, after.width) <= rest.width();
            };
            bool vertical = rest.width() >= rest.height();
            if (!fits(vertical) && fits(!vertical))
            {
                vertical = !vertical;
            }
            if ((vertical ? rest.width() : rest.height()) < (2 * kMinDwindleSlot) + gap)
            {
                break; // too small to halve again: the rest share it
            }
            const std::int32_t room = std::max((vertical ? rest.width() : rest.height()) - gap, 2);
            const std::int32_t first = firstPart(room, 0.5, vertical ? taken.width : taken.height,
                                                 vertical ? after.width : after.height);
            const auto [part, left] = splitAt(rest, vertical, first, gap);
            out.push_back(part);
            rest = left;
        }
        if (out.size() + 1 == count)
        {
            out.push_back(rest);
            return out;
        }
        const bool vertical = rest.width() >= rest.height();
        const auto shared =
            slices(rest, count - out.size(), vertical, gap, along(minimums, out.size(), count, vertical));
        out.insert(out.end(), shared.begin(), shared.end());
        return out;
    }
    case LayoutKind::Floating:
        break;
    }
    return {};
}

Rect fitted(const Rect& slot, const Size& minimum, const Rect& area) noexcept
{
    const std::int32_t width = std::max(slot.width(), minimum.width);
    const std::int32_t height = std::max(slot.height(), minimum.height);
    if (width == slot.width() && height == slot.height())
    {
        return slot;
    }
    Rect out;
    out.left = area.width() > width ? std::clamp(slot.left, area.left, area.right - width) : area.left;
    out.top = area.height() > height ? std::clamp(slot.top, area.top, area.bottom - height) : area.top;
    out.right = out.left + width;
    out.bottom = out.top + height;
    return out;
}

Rect slideFrame(const Rect& from, const Rect& to, double t) noexcept
{
    if (!(t < 1.0)) // NaN too
    {
        return to;
    }
    t = std::max(t, 0.0);
    const double rest = 1.0 - t;
    const double eased = 1.0 - (rest * rest * rest); // ease-out cubic
    const auto mix = [eased](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(std::lround(a + ((static_cast<double>(b) - a) * eased)));
    };
    const std::int32_t left = mix(from.left, to.left);
    const std::int32_t top = mix(from.top, to.top);
    return {left, top, left + std::min(from.width(), to.width()), top + std::min(from.height(), to.height())};
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
