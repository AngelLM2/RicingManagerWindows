#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Value.hpp"
#include "rmw/json/Json.hpp"

// domain::Value <-> JSON: settings and widget values cross processes as JSON (the bar plan, the
// plugin host's messages).
namespace rmw::json
{

inline constexpr std::size_t kValueMaxDepth = 16;

[[nodiscard]] Json fromValue(const domain::Value& value);
// Fails beyond kValueMaxDepth levels (the JSON limits already bound the size).
[[nodiscard]] Result<domain::Value> toValue(const Json& json, std::size_t depth = 0);

} // namespace rmw::json
