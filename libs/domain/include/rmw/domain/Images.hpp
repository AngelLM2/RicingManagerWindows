#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Color.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rmw::domain
{

// An uncompressed 24-bit BMP filled with one color (for kind = "color" next to image monitors).
// Sides 1..16384; the file stays within limits::kImageMaxBytes.
[[nodiscard]] Result<std::vector<std::byte>> makeSolidBmp(Color color, std::uint32_t width,
                                                          std::uint32_t height);

// The smallest width:height with the same aspect ratio (1920x1080 -> 16x9), so a solid image
// scaled by "fit" covers the monitor exactly without being monitor-sized.
struct Size
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
[[nodiscard]] Size reducedAspect(std::uint32_t width, std::uint32_t height) noexcept;

// The pixel size a PNG or JPEG declares in its header, read from the file's first bytes without
// decoding: a small file can declare a huge image (a decompression bomb). nullopt when the bytes
// are neither, or the header is cut short.
[[nodiscard]] std::optional<Size> rasterImageSize(std::span<const std::byte> head) noexcept;

} // namespace rmw::domain
