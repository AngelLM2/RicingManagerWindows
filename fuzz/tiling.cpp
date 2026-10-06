// The tiler (M13): the plan the core writes and the list of windows a tiler that ended badly left
// (files in the user's folder, read like any input), and the model of workspaces driven by whatever
// the bytes say. The first byte picks which.

#include "rmw/tiling/Desktop.hpp"
#include "rmw/tiling/Layout.hpp"
#include "rmw/tiling/Plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace
{

using namespace rmw::tiling;

void plan(std::string_view text)
{
    const auto parsed = parsePlan(text);
    if (!parsed)
    {
        return;
    }
    [[maybe_unused]] const auto again = parsePlan(toJson(*parsed));
    const LayoutOptions options = layoutOptions(parsed->settings, 1.5);
    for (const auto layout : {LayoutKind::Dwindle, LayoutKind::MasterStack, LayoutKind::Columns, LayoutKind::Monocle})
    {
        const Rect area{-1920, 0, 0, 1040};
        for (const auto& rect : arrange(layout, area, 7, options))
        {
            if (rect.empty() || rect.left < area.left || rect.right > area.right)
            {
                std::abort();
            }
        }
    }
    for (std::uint32_t vk = 0; vk < 256; vk += 7)
    {
        [[maybe_unused]] const auto match = matchKey(parsed->settings.keys, vk & 0xFU, vk);
    }
    [[maybe_unused]] const auto* rule = matchRule(parsed->settings.rules, {"a.exe", "Class", "Title"});
}

// Each byte is an operation on a desktop of two monitors; afterwards no window is in two places and
// every placement is inside its monitor.
void model(const std::uint8_t* data, std::size_t size)
{
    Desktop desktop(4, LayoutKind::Dwindle);
    std::vector<Monitor> monitors = {Monitor{"A", Rect{0, 0, 1920, 1040}, Rect{0, 0, 1920, 1080}, 1.0},
                                     Monitor{"B", Rect{1920, 0, 3200, 1024}, Rect{1920, 0, 3200, 1024}, 1.0}};
    desktop.setMonitors(monitors);
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::uint8_t byte = data[i];
        const WindowId window = (byte >> 3U) % 12U + 1U;
        const int number = static_cast<int>(byte >> 5U);
        switch (byte & 7U)
        {
        case 0:
            desktop.add(window, byte & 1U, (byte & 0x40U) != 0 ? std::optional(number % 5) : std::nullopt,
                        (byte & 0x80U) != 0);
            break;
        case 1:
            desktop.remove(window);
            break;
        case 2:
            desktop.goTo(byte & 1U, number % 5);
            break;
        case 3:
            desktop.sendTo(window, number % 5);
            break;
        case 4:
            desktop.moveToMonitor(window, (byte >> 4U) & 1U);
            break;
        case 5:
            if (monitors.size() == 2)
            {
                monitors.pop_back();
            }
            else
            {
                monitors.push_back(Monitor{"B", Rect{1920, 0, 3200, 1024}, Rect{1920, 0, 3200, 1024}, 1.0});
            }
            desktop.setMonitors(monitors);
            break;
        case 6:
            desktop.setWorkspaceCount(number % 10);
            break;
        default:
            if (auto place = desktop.find(window))
            {
                Workspace& workspace = desktop.workspace(*place);
                if ((byte & 0x10U) != 0)
                {
                    workspace.toggleFloating(window);
                }
                else
                {
                    workspace.cycleLayout();
                }
                workspace.exchange(window, static_cast<Direction>(number % 4), desktop.area(place->monitor),
                               LayoutOptions{});
            }
            break;
        }
    }
    const auto all = desktop.all();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (std::count(all.begin(), all.end(), all[i]) != 1 || !desktop.find(all[i]))
        {
            std::abort();
        }
    }
    for (std::size_t m = 0; m < desktop.monitorCount(); ++m)
    {
        const Rect& area = desktop.area(m);
        for (const auto& [id, rect] : desktop.activeWorkspace(m).placements(area, LayoutOptions{}))
        {
            if (rect.left < area.left || rect.right > area.right || rect.top < area.top || rect.bottom > area.bottom)
            {
                std::abort();
            }
        }
    }
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data) + 1, size - 1);
    switch (data[0] % 3)
    {
    case 0:
        plan(text);
        break;
    case 1:
        if (auto windows = parseOriginals(text))
        {
            [[maybe_unused]] const auto again = parseOriginals(toJson(*windows));
        }
        break;
    default:
        model(data + 1, size - 1);
        break;
    }
    return 0;
}
