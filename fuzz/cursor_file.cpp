// .cur and .ani files: every process of the system reads an applied cursor, so the structure is
// checked by our own parser first (docs/06 §4). The first byte picks the extension.

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
    [[maybe_unused]] const auto format =
        rmw::domain::validateCursorFile(file, (data[0] & 1) != 0 ? "ani" : "cur");
    return 0;
}
