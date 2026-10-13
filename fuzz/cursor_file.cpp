// .cur and .ani files: every process of the system reads an applied cursor, so the structure is
// checked by our own parser first (docs/06 §4). The first byte picks the extension. The same bytes go
// through the .ico check too (M14: icon packs; Explorer draws them).

#include "rmw/domain/CursorFiles.hpp"

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
    const std::span<const std::byte> file(bytes + 1, size - 1);
    const auto* extension = (data[0] & 1) != 0 ? "ani" : "cur";
    [[maybe_unused]] const auto format = rmw::domain::validateCursorFile(file, extension);
    [[maybe_unused]] const auto icon = rmw::domain::validateIconFile(file);
    // v1.9: the same file drawn smaller (pointers below 32 px) writes into its own copy.
    [[maybe_unused]] const auto smaller = rmw::domain::shrinkCursorFile(file, extension, 0.75);
    return 0;
}
