#include "rmw/tiling/Workspace.hpp"

#include <algorithm>

namespace rmw::tiling
{

void Workspace::add(WindowId window, bool floating)
{
    if (!contains(window))
    {
        m_windows.push_back(window);
        if (floating)
        {
            m_floating.push_back(window);
        }
    }
    m_focused = window;
}

bool Workspace::remove(WindowId window)
{
    const bool removed = std::erase(m_windows, window) > 0;
    std::erase(m_floating, window);
    if (m_focused == window)
    {
        m_focused = m_windows.empty() ? std::nullopt : std::optional(m_windows.back());
    }
    return removed;
}

bool Workspace::contains(WindowId window) const noexcept
{
    return std::ranges::find(m_windows, window) != m_windows.end();
}

void Workspace::focus(WindowId window) noexcept
{
    if (contains(window))
    {
        m_focused = window;
    }
}

void Workspace::toggleFloating(WindowId window)
{
    if (!contains(window))
    {
        return;
    }
    if (floating(window))
    {
        std::erase(m_floating, window);
    }
    else
    {
        m_floating.push_back(window);
    }
}

bool Workspace::floating(WindowId window) const noexcept
{
    return std::ranges::find(m_floating, window) != m_floating.end();
}

void Workspace::setLayout(LayoutKind layout) noexcept
{
    m_layout = layout;
    m_monocle = false;
}

void Workspace::cycleLayout() noexcept
{
    m_layout = nextLayout(m_layout);
    m_monocle = false;
}

std::vector<WindowId> Workspace::tiled() const
{
    std::vector<WindowId> out;
    out.reserve(m_windows.size());
    for (const WindowId window : m_windows)
    {
        if (!floating(window))
        {
            out.push_back(window);
        }
    }
    return out;
}

std::vector<Size> Workspace::minimumsOf(std::span<const WindowId> windows, const LayoutOptions& options)
{
    std::vector<Size> out;
    if (options.minimums == nullptr || options.minimums->empty())
    {
        return out;
    }
    out.reserve(windows.size());
    for (const WindowId window : windows)
    {
        const auto found = options.minimums->find(window);
        out.push_back(found != options.minimums->end() ? found->second : Size{});
    }
    return out;
}

std::vector<std::pair<WindowId, Rect>> Workspace::placements(const Rect& area, const LayoutOptions& options) const
{
    const auto windows = tiled();
    const auto rects = arrange(layout(), area, windows.size(), options, minimumsOf(windows, options));
    std::vector<std::pair<WindowId, Rect>> out;
    out.reserve(rects.size());
    for (std::size_t i = 0; i < rects.size(); ++i)
    {
        out.emplace_back(windows[i], rects[i]);
    }
    return out;
}

std::optional<WindowId> Workspace::neighborOf(WindowId window, Direction direction, const Rect& area,
                                              const LayoutOptions& options) const
{
    const auto windows = tiled();
    const auto at = std::ranges::find(windows, window);
    if (at == windows.end())
    {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(at - windows.begin());
    if (layout() == LayoutKind::Monocle)
    {
        // One window shows at a time: left/up is the one before, right/down the one after.
        const bool back = direction == Direction::Left || direction == Direction::Up;
        if (back ? index == 0 : index + 1 >= windows.size())
        {
            return std::nullopt;
        }
        return windows[back ? index - 1 : index + 1];
    }
    const auto rects = arrange(layout(), area, windows.size(), options, minimumsOf(windows, options));
    if (const auto next = tiling::neighbor(rects, index, direction))
    {
        return windows[*next];
    }
    return std::nullopt;
}

bool Workspace::exchange(WindowId window, Direction direction, const Rect& area, const LayoutOptions& options)
{
    const auto other = neighborOf(window, direction, area, options);
    return other && exchange(window, *other);
}

bool Workspace::exchange(WindowId a, WindowId b)
{
    const auto first = std::ranges::find(m_windows, a);
    const auto second = std::ranges::find(m_windows, b);
    if (first == m_windows.end() || second == m_windows.end() || first == second)
    {
        return false;
    }
    std::iter_swap(first, second);
    return true;
}

void Workspace::promote(WindowId window)
{
    const auto at = std::ranges::find(m_windows, window);
    if (at != m_windows.end())
    {
        std::rotate(m_windows.begin(), at, at + 1);
    }
}

} // namespace rmw::tiling
