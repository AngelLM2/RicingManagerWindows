#include "rmw/modules/Taskbar.hpp"

#include "rmw/modules/RegistryFields.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace rmw::modules
{
namespace
{

constexpr std::string_view kAdvanced = R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)";
constexpr std::string_view kSearch = R"(Software\Microsoft\Windows\CurrentVersion\Search)";

constexpr std::array<DwordField, 6> kFields = {{
    {"alignment", kAdvanced, "TaskbarAl"},
    {"taskView", kAdvanced, "ShowTaskViewButton"},
    {"search", kSearch, "SearchboxTaskbarMode"},
    // Windows' own Start (M11): its layout, and what "Recommended" shows.
    {"startLayout", kAdvanced, "Start_Layout"},
    {"startRecentFiles", kAdvanced, "Start_TrackDocs"},
    {"startTips", kAdvanced, "Start_IrisRecommendations"},
}};

std::uint32_t searchMode(domain::TaskbarSearch search) noexcept
{
    switch (search)
    {
    case domain::TaskbarSearch::Hidden:
        return 0;
    case domain::TaskbarSearch::Icon:
        return 1;
    case domain::TaskbarSearch::Box:
        return 2;
    case domain::TaskbarSearch::Button:
        return 3;
    }
    return 1;
}

} // namespace

Result<engine::Json> TaskbarLogic::desiredFromRice(const domain::Rice& rice,
                                                   engine::IAssetSink& /*assets*/) const
{
    engine::Json desired = engine::Json::object();
    // A bar that replaces the native taskbar needs its space: auto-hide gives it back (ADR-0003),
    // and as a field of this module it is undone like any other change.
    const bool replaced = std::ranges::any_of(
        rice.bars, [](const domain::BarConfig& bar) { return domain::withStyle(bar).hideNativeTaskbar; });
    if (replaced)
    {
        desired["autohide"] = true;
    }
    // Windows' own Start (M11): what its settings allow.
    if (rice.start && rice.start->layout)
    {
        desired["startLayout"] = *rice.start->layout == domain::StartLayout::MorePins             ? 1
                                 : *rice.start->layout == domain::StartLayout::MoreRecommendations ? 2
                                                                                                   : 0;
    }
    if (rice.start && rice.start->recommendations)
    {
        const int shown = *rice.start->recommendations ? 1 : 0;
        desired["startRecentFiles"] = shown;
        desired["startTips"] = shown;
    }
    if (!rice.taskbar)
    {
        return desired;
    }
    const domain::TaskbarConfig& taskbar = *rice.taskbar;
    if (taskbar.alignment)
    {
        desired["alignment"] = *taskbar.alignment == domain::TaskbarAlignment::Left ? 0 : 1;
    }
    if (taskbar.taskView)
    {
        desired["taskView"] = *taskbar.taskView ? 1 : 0;
    }
    if (taskbar.search)
    {
        desired["search"] = searchMode(*taskbar.search);
    }
    if (taskbar.autohide && !replaced)
    {
        desired["autohide"] = *taskbar.autohide;
    }
    return desired;
}

Result<engine::CaptureResult> TaskbarSystem::capture()
{
    engine::CaptureResult result;
    for (const auto& field : kFields)
    {
        auto value = readDwordField(m_registry, field);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[std::string(field.field)] = std::move(*value);
    }
    auto autohide = m_shell.taskbarAutoHide();
    if (!autohide)
    {
        return std::unexpected(std::move(autohide.error()));
    }
    result.state["autohide"] = *autohide;
    return result;
}

Result<void> TaskbarSystem::apply(const engine::Json& desired)
{
    bool registryChanged = false;
    for (const auto& field : kFields)
    {
        const auto value = desired.find(field.field);
        if (value == desired.end())
        {
            continue;
        }
        if (auto written = writeDwordField(m_registry, field, *value); !written)
        {
            return written;
        }
        registryChanged = true;
    }
    if (registryChanged)
    {
        // Explorer re-reads the Advanced/Search values on this broadcast (ADR-0003).
        if (auto broadcast = m_shell.broadcastSettingChange("TraySettings"); !broadcast)
        {
            return broadcast;
        }
    }
    if (const auto autohide = desired.find("autohide"); autohide != desired.end())
    {
        if (!autohide->is_boolean())
        {
            return fail(ErrorCode::InvalidArgument, "field 'autohide' needs true or false");
        }
        if (auto set = m_shell.setTaskbarAutoHide(autohide->get<bool>()); !set)
        {
            return set;
        }
    }
    return {};
}

} // namespace rmw::modules
