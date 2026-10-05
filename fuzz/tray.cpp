// The WM_COPYDATA any app may send our tray window (Shell_NotifyIcon and its rectangle queries,
// docs/04 M5): raw bytes from other processes.

#include "rmw/bar/Tray.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    [[maybe_unused]] const auto update = rmw::bar::tray::decodeUpdate(bytes);
    [[maybe_unused]] const auto query = rmw::bar::tray::decodeRectQuery(bytes);
    return 0;
}
