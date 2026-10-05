#pragma once

#include <string>
#include <vector>

namespace rmw::platform
{

// Other customization tools that are running or installed for this user (docs/03 §5.1): when the
// "original Windows" is captured with them around, it may already be customized, and they may
// change the same settings we do. Names are product names, sorted, without duplicates.
// Read-only: process list and HKCU keys.
[[nodiscard]] std::vector<std::string> detectOtherCustomizers();

} // namespace rmw::platform
