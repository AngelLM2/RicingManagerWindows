#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What the core hands rmw-bar: the rice's [[bar]]s with palette references resolved to colors,
// written to <data>\bar\bars.json. rmw-bar reads it with the JSON limits and checks its shape like
// any other input; a plan of another major version is refused.
namespace rmw::bar
{

inline constexpr int kPlanVersion = 1;

// A plugin whose widget some bar shows: where its files are (rmw-bar reads its manifest and its
// .slint, and checks them, itself).
struct PlanPlugin
{
    std::string id;
    std::filesystem::path folder;
    bool dev = false; // being written (rmw plugin dev): reloaded when its files change

    friend bool operator==(const PlanPlugin&, const PlanPlugin&) = default;
};

// Ricing Manager's Start menu (M11), shown by rmw-bar.
struct StartPlan
{
    bool ours = false;   // the bar's Start button opens ours (else Windows')
    bool winKey = false; // ...and the Win key alone (the user's choice, in settings.toml)
    std::vector<std::string> pinned;
    std::string language = "en"; // its words: "en" or "pt-BR" (the core's choice)

    friend bool operator==(const StartPlan&, const StartPlan&) = default;
};

struct Plan
{
    std::string riceName;
    StartPlan start;
    std::vector<domain::BarConfig> bars; // every ColorRef holds a Color (or nothing)
    std::vector<PlanPlugin> plugins;
};

// Resolves palette references. The rice parser checked the rice's own; a name only a skin uses and
// the rice does not define is left out (the bar's default color shows).
[[nodiscard]] Result<Plan> planFromRice(const domain::Rice& rice);

[[nodiscard]] std::string toJson(const Plan& plan);
[[nodiscard]] Result<Plan> planFromJson(std::string_view text);

// Some bar wants Windows' own taskbar out of the way.
[[nodiscard]] bool hidesNativeTaskbar(const Plan& plan) noexcept;

} // namespace rmw::bar
