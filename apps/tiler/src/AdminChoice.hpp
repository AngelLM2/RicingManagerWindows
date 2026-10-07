#pragma once

#include <filesystem>

namespace rmw::tilerapp
{

// The person's "no" to the advanced mode's UAC prompt (or an elevated copy that could not work), kept
// next to the plan for this sign-in: a tiler started again (the supervisor after a crash, a core started
// again) does not ask again. Turning the advanced mode (or tiling) off forgets it, so turning it on asks
// once more. Only the plain tiler reads and writes it.
[[nodiscard]] bool adminDeclinedThisSession(const std::filesystem::path& planFile);
void rememberAdminDeclined(const std::filesystem::path& planFile);
void forgetAdminDeclined(const std::filesystem::path& planFile);

} // namespace rmw::tilerapp
