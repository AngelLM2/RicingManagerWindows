#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"

#include <string>
#include <string_view>
#include <vector>

// What the core hands rmw-bar: the rice's [[bar]]s with palette references resolved to colors,
// written to <data>\bar\bars.json. rmw-bar reads it with the JSON limits and checks its shape like
// any other input; a plan of another major version is refused.
namespace rmw::bar
{

inline constexpr int kPlanVersion = 1;

struct Plan
{
    std::string riceName;
    std::vector<domain::BarConfig> bars; // every ColorRef holds a Color
};

// Resolves palette references (the parser already checked they exist).
[[nodiscard]] Result<Plan> planFromRice(const domain::Rice& rice);

[[nodiscard]] std::string toJson(const Plan& plan);
[[nodiscard]] Result<Plan> planFromJson(std::string_view text);

// Some bar wants Windows' own taskbar out of the way.
[[nodiscard]] bool hidesNativeTaskbar(const Plan& plan) noexcept;

} // namespace rmw::bar
