#pragma once

#include "Widgets.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace rmw::barapp
{

// The "dock" widget (M10): the rice's pinned apps, then every other app with a window open, one
// icon per app (its windows together). A click opens a pinned app that has no window, brings an
// app's window forward, or goes to its next window; a dot under the icon says it is running.
// `{ id = "dock", pinned = ["Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", ...] }`: the ids
// `rmw apps` lists; only apps the Start menu lists are ever opened.
[[nodiscard]] std::unique_ptr<Provider> makeDockProvider(const bar::Slot& slot, const Instance& instance);

// An app id as `rmw apps` gives it ("{known folder}\\app.exe", an AppUserModelID) in the form a
// window's identity takes (providers::appIdentity: the AppUserModelID, else the executable's
// path), lower case, to compare the two.
[[nodiscard]] std::wstring comparableAppId(std::wstring_view id);

} // namespace rmw::barapp
