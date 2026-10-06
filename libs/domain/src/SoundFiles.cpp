#include "rmw/domain/SoundFiles.hpp"

#include "rmw/base/Limits.hpp"

#include <array>
#include <cstring>
#include <format>
#include <string_view>

namespace rmw::domain
{
namespace
{

constexpr std::size_t kMaxChunks = 256;

Result<WaveFormat> invalid(std::string_view why)
{
    return fail(ErrorCode::InvalidData, std::format("not a usable .wav: {}", why));
}

std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[at]) | (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[at + 3]) << 24U);
}

std::uint16_t u16(std::span<const std::byte> bytes, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(bytes[at]) |
                                      (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U));
}

bool tagIs(std::span<const std::byte> bytes, std::size_t at, std::string_view tag) noexcept
{
    return std::memcmp(bytes.data() + at, tag.data(), 4) == 0;
}

bool playable(std::uint16_t encoding) noexcept
{
    switch (encoding)
    {
    case 0x0001: // PCM
    case 0x0002: // MS ADPCM
    case 0x0003: // IEEE float
    case 0x0006: // A-law
    case 0x0007: // mu-law
    case 0x0011: // IMA ADPCM
    case 0xFFFE: // extensible
        return true;
    default:
        return false;
    }
}

} // namespace

Result<WaveFormat> validateWaveFile(std::span<const std::byte> bytes)
{
    if (bytes.size() > limits::kSoundMaxBytes)
    {
        return invalid(std::format("larger than {} MB", limits::kSoundMaxBytes >> 20U));
    }
    if (bytes.size() < 12 || !tagIs(bytes, 0, "RIFF") || !tagIs(bytes, 8, "WAVE"))
    {
        return invalid("no RIFF/WAVE header");
    }
    const std::uint64_t declared = std::uint64_t{u32(bytes, 4)} + 8;
    if (declared < 12 || declared > bytes.size() + 1) // a last odd byte may be missing
    {
        return invalid("the RIFF size does not match the file");
    }
    const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(declared), bytes.size());
    WaveFormat format;
    bool haveFormat = false;
    bool haveData = false;
    std::size_t at = 12;
    for (std::size_t chunks = 0; at + 8 <= end; ++chunks)
    {
        if (chunks >= kMaxChunks)
        {
            return invalid("too many chunks");
        }
        const std::uint32_t size = u32(bytes, at + 4);
        const std::uint64_t body = at + 8;
        if (body + size > end)
        {
            return invalid("a chunk goes past the end of the file");
        }
        if (tagIs(bytes, at, "fmt "))
        {
            if (haveFormat || size < 16)
            {
                return invalid(R"(a second or too short "fmt " chunk)");
            }
            const auto b = static_cast<std::size_t>(body);
            format.encoding = u16(bytes, b);
            format.channels = u16(bytes, b + 2);
            format.sampleRate = u32(bytes, b + 4);
            const std::uint16_t blockAlign = u16(bytes, b + 12);
            format.bitsPerSample = u16(bytes, b + 14);
            if (!playable(format.encoding))
            {
                return invalid(std::format("format 0x{:04X} is not one Windows plays", format.encoding));
            }
            if (format.channels < 1 || format.channels > 8 || format.sampleRate < 4000 ||
                format.sampleRate > 384000 || blockAlign == 0 || format.bitsPerSample > 64)
            {
                return invalid("channels, sample rate or block size out of range");
            }
            haveFormat = true;
        }
        else if (tagIs(bytes, at, "data"))
        {
            if (!haveFormat)
            {
                return invalid(R"("data" before "fmt ")");
            }
            if (size == 0)
            {
                return invalid("no samples");
            }
            format.dataBytes = size;
            haveData = true;
            break;
        }
        at = static_cast<std::size_t>(body + size + (size & 1U)); // chunks are padded to even sizes
    }
    if (!haveFormat || !haveData)
    {
        return invalid(R"(no "fmt " and "data" chunks)");
    }
    return format;
}

} // namespace rmw::domain
