#pragma once

#include "Widgets.hpp"

// The system widgets: cpu, memory, network, battery, volume, notifications, desktops.
namespace rmw::barapp
{

// nullptr when the slot is not one of them.
[[nodiscard]] std::unique_ptr<Provider> makeSystemProvider(const bar::Slot& slot, const Instance& instance);

} // namespace rmw::barapp
