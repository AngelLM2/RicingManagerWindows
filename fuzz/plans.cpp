// The plans the core writes for its workers (bar, plugin host, video): files in the user's folder,
// so the workers read them like any input. The first byte picks the plan.

#include "rmw/bar/Plan.hpp"
#include "rmw/plugins/HostPlan.hpp"
#include "rmw/video/Plan.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

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
        if (auto plan = rmw::bar::planFromJson(text))
        {
            [[maybe_unused]] const auto again = rmw::bar::planFromJson(rmw::bar::toJson(*plan));
        }
        break;
    case 1: {
        [[maybe_unused]] const auto host = rmw::plugins::hostPlanFromJson(text);
        break;
    }
    default:
        if (auto plan = rmw::video::planFromJson(text))
        {
            [[maybe_unused]] const auto again = rmw::video::planFromJson(rmw::video::toJson(*plan));
        }
        break;
    }
    return 0;
}
