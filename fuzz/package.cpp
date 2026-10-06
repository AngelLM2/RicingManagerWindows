// .rice and .rmwp packages: zips from anyone (docs/06 §4). Every entry's name, size, type and
// content is checked; with no staging folder nothing reaches the disk. The first byte picks the kind.

#include "rmw/package/Package.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    const auto kind = (data[0] & 1) != 0 ? rmw::package::Kind::Plugin : rmw::package::Kind::Rice;
    [[maybe_unused]] const auto imported =
        rmw::package::extract(std::span<const std::byte>(bytes + 1, size - 1), kind, {});
    return 0;
}
