#include "rmw/plugins/Url.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace rmw::plugins
{
namespace
{

constexpr std::string_view kScheme = "https://";

// Headers the host owns (or that would let a plugin smuggle credentials or a second request).
constexpr auto kReservedHeaders = std::to_array<std::string_view>({
    "host",
    "cookie",
    "cookie2",
    "content-length",
    "transfer-encoding",
    "connection",
    "keep-alive",
    "upgrade",
    "te",
    "trailer",
    "expect",
    "proxy-authorization",
    "proxy-connection",
    "origin",
    "referer",
});

char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isPrintableAscii(char c) noexcept
{
    return c > 0x20 && c < 0x7f;
}

} // namespace

Result<HttpsUrl> parseHttpsUrl(std::string_view url)
{
    if (url.size() > kUrlMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("the address is longer than {} bytes", kUrlMaxBytes));
    }
    if (url.size() < kScheme.size() || !std::ranges::equal(url.substr(0, kScheme.size()), kScheme, {}, lower))
    {
        return fail(ErrorCode::InvalidArgument, "plugins reach the network over https:// only");
    }
    if (!std::ranges::all_of(url, isPrintableAscii) || url.find('\\') != std::string_view::npos)
    {
        return fail(ErrorCode::InvalidArgument,
                    "the address has spaces, backslashes or non-ASCII characters");
    }
    std::string_view rest = url.substr(kScheme.size());
    const std::size_t end = std::min(rest.find_first_of("/?#"), rest.size());
    const std::string_view authority = rest.substr(0, end);
    std::string_view target = rest.substr(end);
    if (authority.find('@') != std::string_view::npos)
    {
        return fail(ErrorCode::InvalidArgument, "user names and passwords do not go in the address");
    }
    std::string_view host = authority;
    if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos)
    {
        if (authority.substr(colon + 1) != "443")
        {
            return fail(ErrorCode::InvalidArgument, "plugins reach the default https port (443) only");
        }
        host = authority.substr(0, colon);
    }
    if (host.empty() || !std::ranges::all_of(host, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                   c == '-';
        }))
    {
        return fail(ErrorCode::InvalidArgument, "invalid host in the address");
    }
    if (const std::size_t hash = target.find('#'); hash != std::string_view::npos)
    {
        target = target.substr(0, hash);
    }
    HttpsUrl parsed;
    parsed.host.resize(host.size());
    std::ranges::transform(host, parsed.host.begin(), lower);
    parsed.target = target.empty() || target.front() == '?' ? "/" + std::string(target) : std::string(target);
    return parsed;
}

Result<void> checkHttpHeader(std::string_view name, std::string_view value)
{
    if (name.empty() || name.size() > 64 || !std::ranges::all_of(name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        }))
    {
        return fail(ErrorCode::InvalidArgument, std::format("invalid header name '{}'", name));
    }
    std::string lowered(name);
    std::ranges::transform(lowered, lowered.begin(), lower);
    if (std::ranges::find(kReservedHeaders, lowered) != kReservedHeaders.end() ||
        lowered.starts_with("proxy-") || lowered.starts_with("sec-"))
    {
        return fail(ErrorCode::AccessDenied, std::format("the host sets '{}' itself", name));
    }
    if (value.size() > 1'024 || !std::ranges::all_of(value, [](char c) { return c >= 0x20 && c < 0x7f; }))
    {
        return fail(ErrorCode::InvalidArgument, std::format("invalid value for header '{}'", name));
    }
    return {};
}

} // namespace rmw::plugins
