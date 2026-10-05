#pragma once

#include "rmw/base/Error.hpp"

#include <filesystem>
#include <string>

// The logon entry (docs/03 §2.2): HKCU\Software\Microsoft\Windows\CurrentVersion\Run runs
// `rmw-core --startup`. Written only when the user asks; if the user turned it off in Task
// Manager (StartupApproved), we report it and never switch it back on ourselves.
namespace rmw::platform
{

struct AutostartState
{
    bool registered = false;     // our Run value exists
    bool current = false;        // ...and starts this executable
    bool disabledByUser = false; // switched off in Task Manager
    std::wstring command;        // what the Run value holds
};

[[nodiscard]] std::wstring autostartCommand(const std::filesystem::path& executable);
[[nodiscard]] Result<AutostartState> autostartState(const std::filesystem::path& executable);
[[nodiscard]] Result<void> setAutostart(bool enabled, const std::filesystem::path& executable);

} // namespace rmw::platform
