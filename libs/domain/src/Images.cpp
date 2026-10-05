#include "rmw/domain/Images.hpp"

#include "rmw/base/Limits.hpp"

#include <algorithm>
#include <format>
#include <numeric>

namespace rmw::domain
{
namespace
{

void put16(std::vector<std::byte>& out, std::size_t at, std::uint16_t value)
{
    out[at] = std::byte(value & 0xFF);
    out[at + 1] = std::byte(value >> 8);
}

void put32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value)
{
    put16(out, at, static_cast<std::uint16_t>(value & 0xFFFF));
    put16(out, at + 2, static_cast<std::uint16_t>(value >> 16));
}

} // namespace

Result<std::vector<std::byte>> makeSolidBmp(Color color, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0 || width > limits::kImageMaxSide || height > limits::kImageMaxSide)
    {
        return fail(ErrorCode::InvalidArgument, std::format("image size {}x{} out of range", width, height));
    }
    constexpr std::uint32_t kHeaders = 14 + 40;
    const std::uint64_t rowBytes = ((std::uint64_t{width} * 3) + 3) / 4 * 4;
    const std::uint64_t total = kHeaders + rowBytes * height;
    if (total > limits::kImageMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded, std::format("a {}x{} image would be too large", width, height));
    }

    std::vector<std::byte> out(static_cast<std::size_t>(total), std::byte{0});
    // BITMAPFILEHEADER
    out[0] = std::byte{'B'};
    out[1] = std::byte{'M'};
    put32(out, 2, static_cast<std::uint32_t>(total));
    put32(out, 10, kHeaders);
    // BITMAPINFOHEADER
    put32(out, 14, 40);
    put32(out, 18, width);
    put32(out, 22, height);
    put16(out, 26, 1);  // planes
    put16(out, 28, 24); // bits per pixel
    put32(out, 34, static_cast<std::uint32_t>(rowBytes * height));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        std::size_t at = kHeaders + static_cast<std::size_t>(rowBytes * y);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            out[at++] = std::byte{color.b};
            out[at++] = std::byte{color.g};
            out[at++] = std::byte{color.r};
        }
    }
    return out;
}

Size reducedAspect(std::uint32_t width, std::uint32_t height) noexcept
{
    if (width == 0 || height == 0)
    {
        return {1, 1};
    }
    const std::uint32_t divisor = std::gcd(width, height);
    Size size{width / divisor, height / divisor};
    // Odd ratios (e.g. 1366x768 -> 683x384) are fine; keep them under 1024 px either way.
    while (size.width > 1024 || size.height > 1024)
    {
        size.width = (size.width + 1) / 2;
        size.height = (size.height + 1) / 2;
    }
    return size;
}

std::optional<Size> rasterImageSize(std::span<const std::byte> head) noexcept
{
    const auto byte = [&](std::size_t i) { return static_cast<std::uint32_t>(head[i]); };
    const auto big16 = [&](std::size_t i) { return (byte(i) << 8) | byte(i + 1); };
    const auto big32 = [&](std::size_t i) { return (big16(i) << 16) | big16(i + 2); };
    constexpr std::uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (head.size() >= 24 &&
        std::equal(std::begin(kPng), std::end(kPng), head.begin(),
                   [](std::uint8_t a, std::byte b) { return a == static_cast<std::uint8_t>(b); }))
    {
        // The IHDR chunk comes first: length, "IHDR", width, height (big-endian).
        if (byte(12) != 'I' || byte(13) != 'H' || byte(14) != 'D' || byte(15) != 'R')
        {
            return std::nullopt;
        }
        return Size{big32(16), big32(20)};
    }
    if (head.size() >= 4 && byte(0) == 0xFF && byte(1) == 0xD8)
    {
        // JPEG: walk the segments to the first start-of-frame (SOF0..SOF15 but DHT, JPG and DAC).
        std::size_t at = 2;
        while (at + 9 <= head.size())
        {
            if (byte(at) != 0xFF)
            {
                return std::nullopt;
            }
            const std::uint32_t marker = byte(at + 1);
            if (marker == 0xFF)
            {
                ++at; // fill byte
                continue;
            }
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
            {
                return Size{big16(at + 7), big16(at + 5)};
            }
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
            {
                at += 2; // markers without a length
                continue;
            }
            at += 2 + big16(at + 2);
        }
    }
    return std::nullopt;
}

} // namespace rmw::domain
