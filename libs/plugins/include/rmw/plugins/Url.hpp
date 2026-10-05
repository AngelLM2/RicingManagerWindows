#pragma once

#include "rmw/base/Error.hpp"

#include <string>
#include <string_view>

namespace rmw::plugins
{

inline constexpr std::size_t kUrlMaxBytes = 2'048;

// "https://api.example.com/v1/x?q=1" as the plugin host sends it: HTTPS on the default port only,
// no user name or password, no fragment, printable ASCII only (anything else percent-encoded).
struct HttpsUrl
{
    std::string host;   // lower-case
    std::string target; // "/v1/x?q=1" ("/" when empty)
};

[[nodiscard]] Result<HttpsUrl> parseHttpsUrl(std::string_view url);

// A request header a plugin may set: a token name that is not about the connection, the host,
// cookies or the body's framing, with a short printable value.
[[nodiscard]] Result<void> checkHttpHeader(std::string_view name, std::string_view value);

} // namespace rmw::plugins
