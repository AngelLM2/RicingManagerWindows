#include "rmw/video/Plan.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"

#include <algorithm>
#include <format>

namespace rmw::video
{
namespace
{

using json::Json;

constexpr std::uint32_t kMaxFps = 240;
constexpr std::size_t kMaxPathBytes = 4096;

std::optional<domain::WallpaperFit> fitFromName(std::string_view name)
{
    for (const auto fit : {domain::WallpaperFit::Fill, domain::WallpaperFit::Fit,
                           domain::WallpaperFit::Stretch, domain::WallpaperFit::Center})
    {
        if (domain::toString(fit) == name)
        {
            return fit;
        }
    }
    return std::nullopt;
}

Result<Plan> bad(std::string_view what)
{
    return fail(ErrorCode::InvalidData, std::format("video plan: {}", what));
}

} // namespace

Policy policyOf(const domain::WallpaperPolicy& policy)
{
    Policy out;
    out.pauseOnFullscreen = policy.pauseOnFullscreen.value_or(true);
    out.pauseOnBattery = policy.pauseOnBattery.value_or(true);
    out.pauseWhenCovered = policy.pauseWhenCovered.value_or(true);
    out.fpsLimit = policy.fpsLimit.value_or(30);
    out.mute = policy.mute.value_or(true);
    return out;
}

bool Plan::hasVideo() const noexcept
{
    return std::ranges::any_of(entries, &Entry::isVideo);
}

std::vector<int> videoFor(const Plan& plan, std::span<const domain::MonitorFacts> monitors)
{
    std::vector<domain::MonitorSelector> rules;
    rules.reserve(plan.entries.size());
    for (const auto& entry : plan.entries)
    {
        rules.push_back(entry.monitor);
    }
    auto chosen = domain::resolveMonitorRules(rules, monitors);
    for (int& index : chosen)
    {
        if (index >= 0 && !plan.entries[static_cast<std::size_t>(index)].isVideo())
        {
            index = -1; // an image or a color: Windows shows it, nothing to play
        }
    }
    return chosen;
}

std::string toJson(const Plan& plan)
{
    Json entries = Json::array();
    for (const auto& entry : plan.entries)
    {
        entries.push_back({{"monitor", entry.monitor.toString()},
                           {"file", utf16To8Lossy(entry.file.native())},
                           {"fit", std::string(domain::toString(entry.fit))}});
    }
    return json::dump(Json{{"version", kPlanVersion},
                           {"entries", std::move(entries)},
                           {"policy",
                            {{"pause_on_fullscreen", plan.policy.pauseOnFullscreen},
                             {"pause_on_battery", plan.policy.pauseOnBattery},
                             {"pause_when_covered", plan.policy.pauseWhenCovered},
                             {"fps_limit", plan.policy.fpsLimit},
                             {"mute", plan.policy.mute}}}});
}

Result<Plan> planFromJson(std::string_view text)
{
    auto parsed = json::parse(text);
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    if (!root.is_object() || json::intOr(root, "version", 0) != kPlanVersion)
    {
        return bad("unknown version");
    }
    const Json entries = json::memberOr(root, "entries", Json());
    if (!entries.is_array() || entries.size() > kMaxEntries)
    {
        return bad("expected a list of entries");
    }
    Plan plan;
    for (const auto& item : entries)
    {
        if (!item.is_object())
        {
            return bad("expected an entry");
        }
        auto monitor = domain::MonitorSelector::parse(json::stringOr(item, "monitor", ""));
        const auto fit = fitFromName(json::stringOr(item, "fit", "fill"));
        const std::string file = json::stringOr(item, "file", "");
        if (!monitor || !fit || file.size() > kMaxPathBytes)
        {
            return bad("invalid entry");
        }
        Entry entry;
        entry.monitor = std::move(*monitor);
        entry.fit = *fit;
        if (!file.empty())
        {
            entry.file = std::filesystem::path(utf8To16Lossy(file));
            if (!entry.file.is_absolute())
            {
                return bad("a video's path must be absolute");
            }
        }
        plan.entries.push_back(std::move(entry));
    }
    const Json policy = json::memberOr(root, "policy", Json::object());
    plan.policy.pauseOnFullscreen = json::boolOr(policy, "pause_on_fullscreen", true);
    plan.policy.pauseOnBattery = json::boolOr(policy, "pause_on_battery", true);
    plan.policy.pauseWhenCovered = json::boolOr(policy, "pause_when_covered", true);
    plan.policy.fpsLimit = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(json::intOr(policy, "fps_limit", 30), 0, kMaxFps));
    plan.policy.mute = json::boolOr(policy, "mute", true);
    return plan;
}

} // namespace rmw::video
