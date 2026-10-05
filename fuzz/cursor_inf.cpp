// The install.inf of a cursor pack (docs/06 §4).

#include "rmw/domain/CursorFiles.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    [[maybe_unused]] const auto scheme = rmw::domain::parseCursorInf(text);
    return 0;
}
