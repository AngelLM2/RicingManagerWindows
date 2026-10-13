#pragma once

// Byte-level samples of .cur and .ani files for tests (valid by construction, easy to break).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rmw::test
{

class Writer
{
public:
    Writer& u8(std::uint8_t v)
    {
        bytes.push_back(std::byte{v});
        return *this;
    }
    Writer& u16(std::uint16_t v)
    {
        return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8));
    }
    Writer& u32(std::uint32_t v)
    {
        return u16(static_cast<std::uint16_t>(v)).u16(static_cast<std::uint16_t>(v >> 16));
    }
    Writer& tag(std::string_view fourcc)
    {
        for (const char c : fourcc)
        {
            u8(static_cast<std::uint8_t>(c));
        }
        return *this;
    }
    Writer& append(const std::vector<std::byte>& other)
    {
        bytes.insert(bytes.end(), other.begin(), other.end());
        return *this;
    }
    Writer& zeros(std::size_t count)
    {
        bytes.insert(bytes.end(), count, std::byte{0});
        return *this;
    }

    std::vector<std::byte> bytes;
};

// A 32x32 monochrome DIB: header + 2-color palette + XOR mask + AND mask.
inline std::vector<std::byte> monochromeImage(std::int32_t width = 32, std::int32_t height = 64,
                                              std::uint16_t bits = 1)
{
    Writer w;
    w.u32(40).u32(static_cast<std::uint32_t>(width)).u32(static_cast<std::uint32_t>(height)).u16(1).u16(bits);
    w.u32(0).u32(0).u32(0).u32(0).u32(0).u32(0); // compression .. important colors
    w.zeros(8);                                  // palette
    w.zeros(std::size_t{4} * 32 * 2);            // XOR + AND rows (4 bytes each)
    return w.bytes;
}

// A 32x32 32-bit DIB, opaque white everywhere (alpha 255), with its AND mask (all drawn).
inline std::vector<std::byte> colorImage()
{
    Writer w;
    w.u32(40).u32(32).u32(64).u16(1).u16(32);
    w.u32(0).u32(0).u32(0).u32(0).u32(0).u32(0);
    for (int i = 0; i < 32 * 32; ++i)
    {
        w.u8(255).u8(255).u8(255).u8(255);
    }
    w.zeros(std::size_t{4} * 32); // AND mask rows (4 bytes each): nothing transparent
    return w.bytes;
}

inline std::vector<std::byte> cursorFile(std::uint16_t type = 2,
                                         const std::vector<std::byte>& image = monochromeImage(),
                                         std::uint32_t offset = 22)
{
    Writer w;
    w.u16(0).u16(type).u16(1);
    w.u8(32).u8(32).u8(0).u8(0).u16(4).u16(4).u32(static_cast<std::uint32_t>(image.size())).u32(offset);
    w.append(image);
    return w.bytes;
}

struct AniOptions
{
    std::uint32_t frames = 2;
    std::uint32_t steps = 2;
    std::uint32_t storedFrames = 2;
    std::uint32_t flags = 1;
    // The initializer keeps designated initializers that omit it warning-free.
    std::vector<std::uint32_t> sequence = {}; // NOLINT(readability-redundant-member-init)
};

inline std::vector<std::byte> aniFile(const AniOptions& options = {})
{
    Writer body;
    body.tag("ACON");
    body.tag("anih")
        .u32(36)
        .u32(36)
        .u32(options.frames)
        .u32(options.steps)
        .zeros(16)
        .u32(10)
        .u32(options.flags);
    Writer list;
    list.tag("fram");
    const auto frame = cursorFile(1);
    for (std::uint32_t i = 0; i < options.storedFrames; ++i)
    {
        list.tag("icon").u32(static_cast<std::uint32_t>(frame.size())).append(frame);
    }
    body.tag("LIST").u32(static_cast<std::uint32_t>(list.bytes.size())).append(list.bytes);
    if (!options.sequence.empty())
    {
        body.tag("seq ").u32(static_cast<std::uint32_t>(options.sequence.size() * 4));
        for (const auto index : options.sequence)
        {
            body.u32(index);
        }
    }
    Writer file;
    file.tag("RIFF").u32(static_cast<std::uint32_t>(body.bytes.size())).append(body.bytes);
    return file.bytes;
}

} // namespace rmw::test
