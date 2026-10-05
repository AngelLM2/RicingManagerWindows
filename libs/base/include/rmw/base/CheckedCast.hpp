#pragma once

#include "rmw/base/FailFast.hpp"

#include <concepts>
#include <optional>
#include <utility>

// Checked integer conversions (docs/08 §3 rule 7) between size_t, UINT, DWORD, int, ...
namespace rmw
{

// Returns the value converted to To, or nullopt if it does not fit.
template <std::integral To, std::integral From>
constexpr std::optional<To> checkedCast(From value) noexcept
{
    if (std::in_range<To>(value))
    {
        return static_cast<To>(value);
    }
    return std::nullopt;
}

// For conversions that can only fail because of a bug: terminates the process (fast fail).
template <std::integral To, std::integral From>
constexpr To narrowOrFail(From value) noexcept
{
    if (!std::in_range<To>(value))
    {
        failFast("integer conversion out of range");
    }
    return static_cast<To>(value);
}

} // namespace rmw
