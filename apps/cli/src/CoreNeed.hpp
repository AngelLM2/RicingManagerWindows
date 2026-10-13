#pragma once

#include <string_view>

namespace rmw::cli
{

// What a command of `rmw` needs of the core. The installer and the uninstaller run `rmw shutdown
// --wait` and `rmw autostart off`: with no core running, those started one (the bar, the tiler, the
// video, the plugins, maybe a UAC prompt) only to end it a moment later (the lifecycle review, v1.8.1).
enum class CoreNeed
{
    None,      // works on files or the registry alone
    IfRunning, // talks to a core that runs; never starts one
    Start,     // starts one when none runs
};

[[nodiscard]] constexpr CoreNeed coreNeed(std::string_view command) noexcept
{
    if (command == "pack" || command == "validate" || command == "autostart")
    {
        return CoreNeed::None;
    }
    if (command == "shutdown")
    {
        return CoreNeed::IfRunning;
    }
    return CoreNeed::Start;
}

} // namespace rmw::cli
