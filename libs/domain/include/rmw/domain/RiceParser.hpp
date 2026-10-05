#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/Rice.hpp"

#include <string_view>

namespace rmw::domain
{

// The version of this build, for `min_app` checks.
SemVer currentAppVersion() noexcept;

struct RiceParseOptions
{
    SemVer appVersion = currentAppVersion();
};

// Reads and validates rice.toml text (untrusted). Every problem lands in `diagnostics` with its
// line, column and key; unknown keys are warnings. Returns InvalidData when there is at least one
// error (the message summarizes them), the rice otherwise. Never throws.
Result<Rice> parseRice(std::string_view text, Diagnostics& diagnostics, const RiceParseOptions& options = {});

} // namespace rmw::domain
