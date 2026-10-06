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

// launchApp() on a thread of its own, for a UI thread: it lists every app first, and an app that asks for
// administrator holds ShellExecuteEx until the person answers the UAC (a bar waiting that long is taken
// for hung). A failure goes to the log, after `who`.
void launchAppInBackground(std::string id, std::string who);

} // namespace rmw::platform
