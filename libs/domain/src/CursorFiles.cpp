#include "rmw/domain/CursorFiles.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/domain/PathRules.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace rmw::domain
{
namespace
{

// ---- little-endian reads with bounds checks ----------------------------------------------------

class Bytes
{
public:
    explicit Bytes(std::span<const std::byte> bytes) noexcept
        : m_bytes(bytes)
    {
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }
    [[nodiscard]] bool has(std::uint64_t offset, std::uint64_t length) const noexcept
    {
        return offset <= m_bytes.size() && length <= m_bytes.size() - offset;
    }
    [[nodiscard]] std::uint8_t u8(std::size_t offset) const noexcept
    {
        return std::to_integer<std::uint8_t>(m_bytes[offset]);
    }
    [[nodiscard]] std::uint16_t u16(std::size_t offset) const noexcept
    {
        return static_cast<std::uint16_t>(u8(offset) | (u8(offset + 1) << 8));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t offset) const noexcept
    {
        return static_cast<std::uint32_t>(u16(offset)) | (static_cast<std::uint32_t>(u16(offset + 2)) << 16);
    }
    [[nodiscard]] std::uint32_t u32be(std::size_t offset) const noexcept
    {
        return (std::uint32_t{u8(offset)} << 24) | (std::uint32_t{u8(offset + 1)} << 16) |
               (std::uint32_t{u8(offset + 2)} << 8) | u8(offset + 3);
    }
    [[nodiscard]] bool tag(std::size_t offset, std::string_view fourcc) const noexcept
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (u8(offset + i) != static_cast<std::uint8_t>(fourcc[i]))
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] std::span<const std::byte> slice(std::size_t offset, std::size_t length) const noexcept
    {
        return m_bytes.subspan(offset, length);
    }

private:
    std::span<const std::byte> m_bytes;
};

Result<CursorFormat> invalid(std::string_view why)
{
    return fail(ErrorCode::InvalidData, std::format("not a valid cursor file: {}", why));
}

constexpr std::size_t kMaxIconEntries = 64;
constexpr std::uint32_t kMaxAniFrames = 1024;
constexpr std::uint32_t kMaxAniSteps = 4096;
constexpr std::size_t kMaxChunks = 16'384;
constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

bool isPng(const Bytes& image) noexcept
{
    if (image.size() < kPngSignature.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < kPngSignature.size(); ++i)
    {
        if (image.u8(i) != kPngSignature[i])
        {
            return false;
        }
    }
    return true;
}

// One image of an icon/cursor directory: a PNG or a DIB (BITMAPINFOHEADER + XOR + AND masks).
Result<void> validateImage(const Bytes& image)
{
    if (isPng(image))
    {
        // PNG: IHDR must follow with 1..256 px sides; the decoder is Windows' job, the bounds ours.
        if (!image.has(8, 25) || !image.tag(12, "IHDR"))
        {
            return fail(ErrorCode::InvalidData, "PNG frame without IHDR");
        }
        const std::uint32_t width = image.u32be(16);
        const std::uint32_t height = image.u32be(20);
        if (width == 0 || height == 0 || width > 256 || height > 256)
        {
            return fail(ErrorCode::InvalidData, "PNG frame size out of range");
        }
        return {};
    }

    if (!image.has(0, 40))
    {
        return fail(ErrorCode::InvalidData, "frame too small for a bitmap header");
    }
    const std::uint32_t headerSize = image.u32(0);
    if (headerSize != 40 && headerSize != 108 && headerSize != 124)
    {
        return fail(ErrorCode::InvalidData, "unknown bitmap header");
    }
    const auto width = static_cast<std::int32_t>(image.u32(4));
    const auto height = static_cast<std::int32_t>(image.u32(8)); // XOR + AND masks: twice the height
    const std::uint16_t planes = image.u16(12);
    const std::uint16_t bitCount = image.u16(14);
    const std::uint32_t colorsUsed = image.u32(32);
    if (width <= 0 || width > 256 || height <= 0 || height > 512 || planes != 1)
    {
        return fail(ErrorCode::InvalidData, "bitmap size out of range");
    }
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32)
    {
        return fail(ErrorCode::InvalidData, "unsupported bit depth");
    }
    const std::uint64_t paletteEntries =
        bitCount <= 8 ? (colorsUsed != 0 ? colorsUsed : (std::uint64_t{1} << bitCount)) : 0;
    if (paletteEntries > 256)
    {
        return fail(ErrorCode::InvalidData, "palette too large");
    }
    const auto rowBytes = [](std::uint64_t pixels, std::uint64_t bits) {
        return ((pixels * bits + 31) / 32) * 4;
    };
    const std::uint64_t imageHeight = static_cast<std::uint64_t>(height) / 2;
    const std::uint64_t required = headerSize + paletteEntries * 4 +
                                   rowBytes(static_cast<std::uint64_t>(width), bitCount) * imageHeight +
                                   rowBytes(static_cast<std::uint64_t>(width), 1) * imageHeight;
    if (required > image.size())
    {
        return fail(ErrorCode::InvalidData, "bitmap data is truncated");
    }
    return {};
}

// ICONDIR with `count` entries; type 1 (icon) or 2 (cursor).
Result<void> validateIconDirectory(const Bytes& file, bool cursorOnly)
{
    if (!file.has(0, 6) || file.u16(0) != 0)
    {
        return fail(ErrorCode::InvalidData, "bad icon directory header");
    }
    const std::uint16_t type = file.u16(2);
    if (type != 2 && (cursorOnly || type != 1))
    {
        return fail(ErrorCode::InvalidData, "not a cursor resource");
    }
    const std::uint16_t count = file.u16(4);
    if (count == 0 || count > kMaxIconEntries)
    {
        return fail(ErrorCode::InvalidData, "bad number of images");
    }
    const std::uint64_t directoryEnd = 6 + std::uint64_t{16} * count;
    if (!file.has(0, directoryEnd))
    {
        return fail(ErrorCode::InvalidData, "truncated icon directory");
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t entry = 6 + 16 * i;
        const std::uint32_t length = file.u32(entry + 8);
        const std::uint32_t offset = file.u32(entry + 12);
        if (offset < directoryEnd || !file.has(offset, length) || length < 8)
        {
            return fail(ErrorCode::InvalidData, "image outside the file");
        }
        if (auto image = validateImage(Bytes(file.slice(offset, length))); !image)
        {
            return image;
        }
    }
    return {};
}

Result<CursorFormat> validateAni(const Bytes& file)
{
    if (!file.has(0, 12) || !file.tag(0, "RIFF") || !file.tag(8, "ACON"))
    {
        return invalid("missing RIFF/ACON header");
    }
    const std::uint64_t riffEnd = std::uint64_t{file.u32(4)} + 8;
    if (riffEnd > file.size())
    {
        return invalid("RIFF size larger than the file");
    }

    std::optional<std::uint32_t> frames;
    std::optional<std::uint32_t> steps;
    std::uint32_t iconFrames = 0;
    std::optional<std::size_t> sequence; // offset of the "seq " data
    std::optional<std::uint32_t> rateSize;
    std::optional<std::uint32_t> sequenceSize;
    std::size_t chunks = 0;

    std::uint64_t position = 12;
    while (position + 8 <= riffEnd)
    {
        if (++chunks > kMaxChunks)
        {
            return invalid("too many chunks");
        }
        const auto at = static_cast<std::size_t>(position);
        const std::uint32_t size = file.u32(at + 4);
        const std::uint64_t data = position + 8;
        if (data + size > riffEnd)
        {
            return invalid("chunk runs past the end");
        }
        const auto dataAt = static_cast<std::size_t>(data);

        if (file.tag(at, "anih"))
        {
            if (size < 36 || file.u32(dataAt) != 36)
            {
                return invalid("bad animation header");
            }
            frames = file.u32(dataAt + 4);
            steps = file.u32(dataAt + 8);
            const std::uint32_t flags = file.u32(dataAt + 32);
            if (*frames == 0 || *frames > kMaxAniFrames || *steps == 0 || *steps > kMaxAniSteps)
            {
                return invalid("frame or step count out of range");
            }
            if ((flags & 1u) == 0)
            {
                return invalid("raw-bitmap animations are not supported");
            }
        }
        else if (file.tag(at, "LIST") && size >= 4 && file.tag(dataAt, "fram"))
        {
            std::uint64_t inner = data + 4;
            const std::uint64_t listEnd = data + size;
            while (inner + 8 <= listEnd)
            {
                if (++chunks > kMaxChunks)
                {
                    return invalid("too many chunks");
                }
                const auto innerAt = static_cast<std::size_t>(inner);
                const std::uint32_t innerSize = file.u32(innerAt + 4);
                if (inner + 8 + innerSize > listEnd)
                {
                    return invalid("frame runs past its list");
                }
                if (file.tag(innerAt, "icon"))
                {
                    if (++iconFrames > kMaxAniFrames)
                    {
                        return invalid("too many frames");
                    }
                    if (auto icon = validateIconDirectory(Bytes(file.slice(innerAt + 8, innerSize)), false);
                        !icon)
                    {
                        return std::unexpected(
                            std::move(icon.error()).withContext(std::format("frame {}", iconFrames)));
                    }
                }
                inner += 8 + innerSize + (innerSize & 1u);
            }
        }
        else if (file.tag(at, "rate"))
        {
            rateSize = size;
        }
        else if (file.tag(at, "seq "))
        {
            sequence = dataAt;
            sequenceSize = size;
        }
        position = data + size + (size & 1u);
    }

    if (!frames || !steps)
    {
        return invalid("missing animation header");
    }
    if (iconFrames != *frames)
    {
        return invalid("frame count does not match the header");
    }
    if (rateSize && *rateSize != *steps * 4)
    {
        return invalid("rate table size does not match the step count");
    }
    if (sequence)
    {
        if (*sequenceSize != *steps * 4)
        {
            return invalid("sequence size does not match the step count");
        }
        for (std::uint32_t i = 0; i < *steps; ++i)
        {
            if (file.u32(*sequence + 4 * std::size_t{i}) >= *frames)
            {
                return invalid("sequence refers to a missing frame");
            }
        }
    }
    return CursorFormat::Animated;
}

// ---- install.inf --------------------------------------------------------------------------------

char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string toLower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view unquote(std::string_view text) noexcept
{
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

// INF fields: comma-separated, commas inside "quotes" do not split.
std::vector<std::string_view> splitFields(std::string_view line)
{
    std::vector<std::string_view> fields;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '"')
        {
            quoted = !quoted;
        }
        else if (line[i] == ',' && !quoted)
        {
            fields.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    fields.push_back(trim(line.substr(start)));
    return fields;
}

// One pass of %name% substitution (no recursion: a hostile file cannot make it grow unbounded
// beyond one level, and the result is capped).
std::string substitute(std::string_view text, const std::map<std::string, std::string>& strings)
{
    std::string out;
    std::size_t i = 0;
    while (i < text.size())
    {
        const std::size_t open = text.find('%', i);
        if (open == std::string_view::npos)
        {
            out += text.substr(i);
            break;
        }
        const std::size_t close = text.find('%', open + 1);
        if (close == std::string_view::npos)
        {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, open - i);
        const auto it = strings.find(toLower(text.substr(open + 1, close - open - 1)));
        out += it != strings.end() ? it->second : std::string(text.substr(open, close - open + 1));
        i = close + 1;
        if (out.size() > limits::kInfMaxBytes)
        {
            break;
        }
    }
    return out;
}

std::string_view baseName(std::string_view path) noexcept
{
    const auto slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

Result<CursorFormat> validateCursorFile(std::span<const std::byte> bytes, std::string_view extension)
{
    if (bytes.size() > limits::kCursorMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded, "cursor file is too large");
    }
    const Bytes file(bytes);
    const std::string ext = toLower(extension);
    if (ext == "cur")
    {
        if (auto directory = validateIconDirectory(file, true); !directory)
        {
            return invalid(directory.error().message());
        }
        return CursorFormat::Static;
    }
    if (ext == "ani")
    {
        return validateAni(file);
    }
    return fail(ErrorCode::InvalidData, "cursor files must be .cur or .ani");
}

Result<std::vector<CursorAssignment>> parseCursorInf(std::string_view text)
{
    if (text.size() > limits::kInfMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded, "install.inf is too large");
    }

    std::map<std::string, std::string> strings;
    std::vector<std::string> addRegSections;
    struct RegLine
    {
        std::string section;
        std::string_view line;
    };
    std::vector<RegLine> regLines;

    std::string section;
    for (std::size_t start = 0; start <= text.size();)
    {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            trim(text.substr(start, (end == std::string_view::npos ? text.size() : end) - start));
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line.front() == ';')
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            section = toLower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto equals = line.find('=');
        if (section == "strings" && equals != std::string_view::npos)
        {
            strings[toLower(trim(line.substr(0, equals)))] = std::string(unquote(line.substr(equals + 1)));
        }
        else if (section == "defaultinstall" && equals != std::string_view::npos &&
                 toLower(trim(line.substr(0, equals))) == "addreg")
        {
            for (const auto name : splitFields(line.substr(equals + 1)))
            {
                addRegSections.push_back(toLower(name));
            }
        }
        else if (toLower(line.substr(0, std::min<std::size_t>(line.size(), 4))) == "hkcu")
        {
            regLines.push_back({section, line});
        }
    }

    std::array<std::string, kCursorRoleCount> files;
    bool found = false;
    for (const auto& [lineSection, line] : regLines)
    {
        if (!addRegSections.empty() && std::ranges::find(addRegSections, lineSection) == addRegSections.end())
        {
            continue;
        }
        const auto fields = splitFields(line);
        if (fields.size() < 5)
        {
            continue;
        }
        const std::string subkey = toLower(substitute(unquote(fields[1]), strings));
        const std::string data = substitute(unquote(fields[4]), strings);
        if (subkey == R"(control panel\cursors\schemes)")
        {
            // The whole scheme: one comma-separated list in Windows' role order.
            std::size_t index = 0;
            for (const auto entry : splitFields(data))
            {
                if (index < kCursorRoleCount)
                {
                    files[index] = std::string(baseName(entry));
                }
                ++index;
            }
            found = true;
        }
        else if (subkey == R"(control panel\cursors)")
        {
            // One value per role.
            const std::string name = toLower(substitute(unquote(fields[2]), strings));
            for (std::size_t i = 0; i < kCursorRoleCount; ++i)
            {
                if (toLower(registryValueName(static_cast<CursorRole>(i))) == name)
                {
                    files[i] = std::string(baseName(data));
                    found = true;
                }
            }
        }
    }
    if (!found)
    {
        return fail(ErrorCode::InvalidData, "install.inf does not define a cursor scheme");
    }

    std::vector<CursorAssignment> assignments;
    for (std::size_t i = 0; i < kCursorRoleCount; ++i)
    {
        if (files[i].empty())
        {
            continue;
        }
        auto name = validatePackagePath(files[i]);
        if (!name)
        {
            return std::unexpected(std::move(name.error()).withContext("install.inf"));
        }
        if (!hasExtension(*name, {"cur", "ani"}))
        {
            return fail(ErrorCode::InvalidData,
                        std::format("install.inf: '{}' is not a .cur or .ani file", *name));
        }
        assignments.push_back({static_cast<CursorRole>(i), std::move(*name)});
    }
    return assignments;
}

} // namespace rmw::domain
