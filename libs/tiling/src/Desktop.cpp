#include "rmw/tiling/Desktop.hpp"

#include "rmw/domain/Settings.hpp"

#include <algorithm>
#include <utility>

namespace rmw::tiling
{

Desktop::Desktop(int workspaces, LayoutKind layout)
    : m_count(std::clamp(workspaces, 1, domain::kMaxWorkspaces))
    , m_layout(layout)
{
}

Desktop::Screen Desktop::makeScreen(Monitor monitor) const
{
    Screen screen{std::move(monitor), {}, 0};
    screen.workspaces.assign(static_cast<std::size_t>(m_count), Workspace(m_layout));
    return screen;
}

Change Desktop::setMonitors(std::vector<Monitor> monitors)
{
    if (monitors.empty())
    {
        return {};
    }
    const auto before = shownWindows();
    std::vector<Screen> screens;
    screens.reserve(monitors.size());
    // By name first; a monitor whose name changed (a DisplayPort one waking up, a driver update) is the
    // one that was in the same place.
    std::vector<std::optional<Screen>> found(monitors.size());
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        const auto old =
            std::ranges::find(m_screens, monitors[i].key, [](const Screen& s) { return s.monitor.key; });
        if (old != m_screens.end())
        {
            found[i] = std::move(*old);
            m_screens.erase(old);
        }
    }
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (found[i])
        {
            continue;
        }
        const auto old =
            std::ranges::find(m_screens, monitors[i].bounds, [](const Screen& s) { return s.monitor.bounds; });
        if (old != m_screens.end())
        {
            found[i] = std::move(*old);
            m_screens.erase(old);
        }
    }
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (found[i])
        {
            Screen kept = std::move(*found[i]);
            kept.monitor = std::move(monitors[i]);
            screens.push_back(std::move(kept));
        }
        else
        {
            screens.push_back(makeScreen(std::move(monitors[i])));
        }
    }
    // What is left in m_screens went away. Its workspaces join the first monitor's of the same number; the
    // one it showed joins the one shown there, so its windows stay in sight.
    Screen& refuge = screens.front();
    for (const Screen& gone : m_screens)
    {
        for (std::size_t w = 0; w < gone.workspaces.size(); ++w)
        {
            const Workspace& workspace = gone.workspaces[w];
            const std::size_t to = std::cmp_equal(w, gone.active) ? static_cast<std::size_t>(refuge.active)
                                                                     : std::min(w, refuge.workspaces.size() - 1);
            for (const WindowId window : workspace.windows())
            {
                refuge.workspaces[to].add(window, workspace.floating(window));
            }
        }
    }
    m_screens = std::move(screens);
    return difference(before, shownWindows());
}

std::vector<Monitor> Desktop::monitors() const
{
    std::vector<Monitor> out;
    out.reserve(m_screens.size());
    for (const Screen& screen : m_screens)
    {
        out.push_back(screen.monitor);
    }
    return out;
}

Change Desktop::setWorkspaceCount(int count)
{
    count = std::clamp(count, 1, domain::kMaxWorkspaces);
    if (count == m_count)
    {
        return {};
    }
    const auto before = shownWindows();
    for (Screen& screen : m_screens)
    {
        if (count > m_count)
        {
            screen.workspaces.resize(static_cast<std::size_t>(count), Workspace(m_layout));
            continue;
        }
        Workspace& last = screen.workspaces[static_cast<std::size_t>(count - 1)];
        for (auto i = static_cast<std::size_t>(count); i < screen.workspaces.size(); ++i)
        {
            for (const WindowId window : screen.workspaces[i].windows())
            {
                last.add(window, screen.workspaces[i].floating(window));
            }
        }
        screen.workspaces.resize(static_cast<std::size_t>(count), Workspace(m_layout));
        screen.active = std::min(screen.active, count - 1);
    }
    m_count = count;
    return difference(before, shownWindows());
}

void Desktop::setLayout(LayoutKind layout)
{
    m_layout = layout;
    for (Screen& screen : m_screens)
    {
        for (Workspace& workspace : screen.workspaces)
        {
            workspace.setLayout(layout);
        }
    }
}

Change Desktop::add(WindowId window, std::size_t monitor, std::optional<int> workspace, bool floating)
{
    if (m_screens.empty() || find(window))
    {
        return {};
    }
    Screen& screen = m_screens[std::min(monitor, m_screens.size() - 1)];
    const int target = std::clamp(workspace.value_or(screen.active), 0, m_count - 1);
    screen.workspaces[static_cast<std::size_t>(target)].add(window, floating);
    Change change;
    if (target == screen.active)
    {
        change.focus = window;
    }
    else
    {
        change.hide.push_back(window);
    }
    return change;
}

bool Desktop::remove(WindowId window)
{
    for (Screen& screen : m_screens)
    {
        for (Workspace& workspace : screen.workspaces)
        {
            if (workspace.remove(window))
            {
                return true;
            }
        }
    }
    return false;
}

std::optional<Place> Desktop::find(WindowId window) const noexcept
{
    for (std::size_t m = 0; m < m_screens.size(); ++m)
    {
        const auto& workspaces = m_screens[m].workspaces;
        for (std::size_t w = 0; w < workspaces.size(); ++w)
        {
            if (workspaces[w].contains(window))
            {
                return Place{m, static_cast<int>(w)};
            }
        }
    }
    return std::nullopt;
}

bool Desktop::shown(WindowId window) const noexcept
{
    const auto place = find(window);
    return place && m_screens[place->monitor].active == place->workspace;
}

int Desktop::active(std::size_t monitor) const noexcept
{
    return monitor < m_screens.size() ? m_screens[monitor].active : 0;
}

Workspace& Desktop::workspace(const Place& place)
{
    return m_screens.at(place.monitor).workspaces.at(static_cast<std::size_t>(place.workspace));
}

const Workspace& Desktop::workspace(const Place& place) const
{
    return m_screens.at(place.monitor).workspaces.at(static_cast<std::size_t>(place.workspace));
}

Workspace& Desktop::activeWorkspace(std::size_t monitor)
{
    return workspace(Place{monitor, active(monitor)});
}

const Rect& Desktop::area(std::size_t monitor) const
{
    return m_screens.at(monitor).monitor.work;
}

std::uint32_t Desktop::occupied(std::size_t monitor) const noexcept
{
    if (monitor >= m_screens.size())
    {
        return 0;
    }
    std::uint32_t bits = 0;
    const auto& workspaces = m_screens[monitor].workspaces;
    for (std::size_t i = 0; i < workspaces.size(); ++i)
    {
        if (!workspaces[i].empty())
        {
            bits |= 1U << i;
        }
    }
    return bits;
}

std::vector<std::pair<WindowId, Rect>> Desktop::placements(const LayoutOptions& options) const
{
    std::vector<std::pair<WindowId, Rect>> out;
    for (const Screen& screen : m_screens)
    {
        const auto placed =
            screen.workspaces[static_cast<std::size_t>(screen.active)].placements(screen.monitor.work, options);
        out.insert(out.end(), placed.begin(), placed.end());
    }
    return out;
}

Change Desktop::goTo(std::size_t monitor, int workspace)
{
    if (monitor >= m_screens.size() || workspace < 0 || workspace >= m_count)
    {
        return {};
    }
    Screen& screen = m_screens[monitor];
    Change change;
    if (workspace != screen.active)
    {
        const Workspace& from = screen.workspaces[static_cast<std::size_t>(screen.active)];
        const Workspace& to = screen.workspaces[static_cast<std::size_t>(workspace)];
        change.hide.assign(from.windows().begin(), from.windows().end());
        change.show.assign(to.windows().begin(), to.windows().end());
        screen.active = workspace;
    }
    change.focus = screen.workspaces[static_cast<std::size_t>(workspace)].focused();
    return change;
}

Change Desktop::sendTo(WindowId window, int workspace)
{
    const auto place = find(window);
    if (!place || workspace < 0 || workspace >= m_count || workspace == place->workspace)
    {
        return {};
    }
    Workspace& from = this->workspace(*place);
    const bool floating = from.floating(window);
    from.remove(window);
    Screen& screen = m_screens[place->monitor];
    screen.workspaces[static_cast<std::size_t>(workspace)].add(window, floating);
    Change change;
    if (workspace != screen.active)
    {
        change.hide.push_back(window);
        if (place->workspace == screen.active)
        {
            change.focus = from.focused();
        }
    }
    else
    {
        change.show.push_back(window);
        change.focus = window;
    }
    return change;
}

Change Desktop::moveToMonitor(WindowId window, std::size_t monitor)
{
    const auto place = find(window);
    if (!place || monitor >= m_screens.size() || monitor == place->monitor)
    {
        return {};
    }
    Workspace& from = workspace(*place);
    const bool floating = from.floating(window);
    from.remove(window);
    activeWorkspace(monitor).add(window, floating);
    Change change;
    if (m_screens[place->monitor].active != place->workspace)
    {
        change.show.push_back(window); // it was hidden: now it is on screen
    }
    change.focus = window;
    return change;
}

std::optional<std::size_t> Desktop::monitorToward(std::size_t from, Direction direction) const
{
    std::vector<Rect> areas;
    areas.reserve(m_screens.size());
    for (const Screen& screen : m_screens)
    {
        areas.push_back(screen.monitor.work);
    }
    return neighbor(areas, from, direction);
}

std::vector<WindowId> Desktop::all() const
{
    std::vector<WindowId> out;
    for (const Screen& screen : m_screens)
    {
        for (const Workspace& workspace : screen.workspaces)
        {
            out.insert(out.end(), workspace.windows().begin(), workspace.windows().end());
        }
    }
    return out;
}

std::vector<WindowId> Desktop::shownWindows() const
{
    std::vector<WindowId> out;
    for (const Screen& screen : m_screens)
    {
        const auto windows = screen.workspaces[static_cast<std::size_t>(screen.active)].windows();
        out.insert(out.end(), windows.begin(), windows.end());
    }
    return out;
}

Change Desktop::difference(const std::vector<WindowId>& before, const std::vector<WindowId>& after)
{
    Change change;
    for (const WindowId window : before)
    {
        if (std::ranges::find(after, window) == after.end())
        {
            change.hide.push_back(window);
        }
    }
    for (const WindowId window : after)
    {
        if (std::ranges::find(before, window) == before.end())
        {
            change.show.push_back(window);
        }
    }
    return change;
}

} // namespace rmw::tiling
