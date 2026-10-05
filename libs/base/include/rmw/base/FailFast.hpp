#pragma once

#include <source_location>
#include <string_view>

namespace rmw
{

// Terminates the process immediately after a best-effort log line. Use only for broken
// invariants (bugs), never for expected failures — those return rmw::Result.
[[noreturn]] void failFast(std::string_view reason,
                           std::source_location where = std::source_location::current()) noexcept;

} // namespace rmw
