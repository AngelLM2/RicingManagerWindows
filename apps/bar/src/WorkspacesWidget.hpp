#pragma once

#include "Widgets.hpp"

#include <functional>
#include <memory>

namespace rmw::barapp
{

// The "workspaces" widget (M13): the tiler's workspaces on this bar's monitor, the ones with windows
// and the one shown (like i3's and Hyprland's); a click goes there. Empty while tiling is off. It
// reads what rmw-tiler shares (rmw/tiling/Shared.hpp) and asks it through its control window.
[[nodiscard]] std::unique_ptr<Provider> makeWorkspacesProvider(const bar::Slot& slot, const Instance& instance);

// The window that shows a bar (set by BarApp): the widget finds its monitor through it.
void setBarWindowLookup(std::function<void*(const slint::interpreter::ComponentInstance*)> lookup);

} // namespace rmw::barapp
