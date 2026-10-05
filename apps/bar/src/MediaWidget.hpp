#pragma once

#include "Widgets.hpp"

namespace rmw::barapp
{

// The "media" widget: what is playing, with previous / play-pause / next.
[[nodiscard]] std::unique_ptr<Provider> makeMediaProvider(const bar::Slot& slot, const Instance& instance);

} // namespace rmw::barapp
