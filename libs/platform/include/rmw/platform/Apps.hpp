#pragma once

#include "rmw/base/Error.hpp"

#include <string>
#include <string_view>
#include <vector>

// The apps the user can open, as the Start menu's "All apps" lists them (shell:AppsFolder): the
// launch.app action of hotkeys, rules and plugins (docs/12 §5). An app is opened by its id from
// that list, never by a path or with arguments: nothing can be run that is not installed.
namespace rmw::platform
{

struct InstalledApp
{
    std::string id;   // AppUserModelID, or the shell's id for a desktop shortcut
    std::string name; // as the Start menu shows it
};

// Sorted by name.
[[nodiscard]] Result<std::vector<InstalledApp>> installedApps();

// Opens the app `id` names, when installedApps() lists it.
[[nodiscard]] Result<void> launchApp(std::string_view id);

} // namespace rmw::platform
