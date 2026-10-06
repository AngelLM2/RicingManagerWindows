// A rice's sounds (M14): a .wav from anyone, played by Windows in many processes once set.

#include "rmw/domain/SoundFiles.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    [[maybe_unused]] const auto format =
        rmw::domain::validateWaveFile(std::span(reinterpret_cast<const std::byte*>(data), size));
    return 0;
}
