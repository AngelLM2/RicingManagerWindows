// rice.toml: any text gives a rice or diagnostics, never a crash (docs/06 §4).

#include "rmw/domain/RiceParser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    rmw::domain::Diagnostics diagnostics;
    [[maybe_unused]] const auto rice = rmw::domain::parseRice(text, diagnostics);
    return 0;
}
