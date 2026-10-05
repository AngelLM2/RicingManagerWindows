// What the plugin host sends the bar over the bridge (hello, instances, set, watch, provide...):
// the bar treats it as input (docs/12 §3).

#include "rmw/bar/Bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    [[maybe_unused]] const auto message = rmw::bar::bridge::decode(text);
    return 0;
}
