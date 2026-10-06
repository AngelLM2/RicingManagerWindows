#pragma once

#include "rmw/base/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// Sound files of a rice (M14) are untrusted and, once set, Windows plays them in many processes:
// bounded, allocation-free checks, and a fuzzing target.
namespace rmw::domain
{

struct WaveFormat
{
    std::uint16_t encoding = 0; // WAVE_FORMAT_* (1 PCM, 3 IEEE float, 0xFFFE extensible...)
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint32_t dataBytes = 0;
};

// Checks a .wav: the RIFF/WAVE header, every chunk inside the file, one "fmt " chunk with a format
// Windows plays (PCM, IEEE float, A-law, mu-law, MS/IMA ADPCM, extensible) and sane numbers, and a
// "data" chunk after it. Does not decode samples.
[[nodiscard]] Result<WaveFormat> validateWaveFile(std::span<const std::byte> bytes);

} // namespace rmw::domain
