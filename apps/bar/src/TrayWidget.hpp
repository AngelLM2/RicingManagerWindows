#pragma once

#include "Widgets.hpp"

#include <memory>

namespace rmw::barapp
{

// The "tray" widget: the system tray's icons (TrayHost). Settings:
//   show = "promoted" (default): the icons Windows shows on its taskbar ("Other system tray icons"
//          in Settings); the others behind a chevron. "all": every icon.
[[nodiscard]] std::unique_ptr<Provider> makeTrayProvider(const bar::Slot& slot, const Instance& instance);

} // namespace rmw::barapp
