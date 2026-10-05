#pragma once

#include "rmw/base/Error.hpp"

// Runtime process mitigations applied at the very start of every executable (docs/06 §5).
namespace rmw::platform
{

// - DLL search restricted to System32 (SetDefaultDllDirectories) — blocks DLL planting.
// - Strict handle checks: using an invalid handle raises an exception instead of
//   silently continuing.
// Further policies (ACG, extension points, image load) are decided per process in spike S6.
Result<void> hardenProcess() noexcept;

} // namespace rmw::platform
