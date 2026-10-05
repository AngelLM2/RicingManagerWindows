// Addresses and headers a plugin asks the host to fetch (docs/12 §6): only https, only the
// domains it declared, nothing local. The first line is a URL, the second a header.

#include "rmw/plugins/Url.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto end = text.find('\n');
    [[maybe_unused]] const auto url = rmw::plugins::parseHttpsUrl(text.substr(0, end));
    if (end != std::string_view::npos)
    {
        const std::string_view header = text.substr(end + 1);
        const auto colon = header.find(':');
        [[maybe_unused]] const auto checked = rmw::plugins::checkHttpHeader(
            header.substr(0, colon), colon == std::string_view::npos ? "" : header.substr(colon + 1));
    }
    return 0;
}
