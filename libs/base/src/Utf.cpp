#include "rmw/base/Utf.hpp"

#include <format>

namespace rmw
{
namespace
{

constexpr char32_t kReplacement = 0xFFFD;
constexpr char32_t kMaxCodePoint = 0x10FFFF;

constexpr bool isSurrogate(char32_t cp) noexcept
{
    return cp >= 0xD800 && cp <= 0xDFFF;
}

struct Decoded
{
    char32_t codePoint;
    std::size_t length; // units consumed (always >= 1)
    bool valid;
};

// Decodes one UTF-8 sequence starting at text[pos] (RFC 3629).
Decoded decodeUtf8(std::string_view text, std::size_t pos) noexcept
{
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80)
    {
        return {lead, 1, true};
    }

    std::size_t length = 0;
    char32_t codePoint = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        codePoint = lead & 0x1Fu;
        minimum = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        codePoint = lead & 0x0Fu;
        minimum = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        codePoint = lead & 0x07u;
        minimum = 0x10000;
    }
    else
    {
        return {kReplacement, 1, false};
    }

    if (text.size() - pos < length)
    {
        return {kReplacement, 1, false};
    }

    for (std::size_t i = 1; i < length; ++i)
    {
        const auto unit = static_cast<unsigned char>(text[pos + i]);
        if ((unit & 0xC0) != 0x80)
        {
            return {kReplacement, 1, false};
        }
        codePoint = (codePoint << 6) | (unit & 0x3Fu);
    }

    if (codePoint < minimum || codePoint > kMaxCodePoint || isSurrogate(codePoint))
    {
        return {kReplacement, 1, false};
    }
    return {codePoint, length, true};
}

// Decodes one UTF-16 code point starting at text[pos].
Decoded decodeUtf16(std::wstring_view text, std::size_t pos) noexcept
{
    const auto unit = static_cast<char32_t>(text[pos]);
    if (!isSurrogate(unit))
    {
        return {unit, 1, true};
    }
    if (unit <= 0xDBFF && pos + 1 < text.size())
    {
        const auto low = static_cast<char32_t>(text[pos + 1]);
        if (low >= 0xDC00 && low <= 0xDFFF)
        {
            return {0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00), 2, true};
        }
    }
    return {kReplacement, 1, false}; // unpaired surrogate
}

void appendUtf16(std::wstring& out, char32_t codePoint)
{
    if (codePoint < 0x10000)
    {
        out.push_back(static_cast<wchar_t>(codePoint));
        return;
    }
    const char32_t value = codePoint - 0x10000;
    out.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
    out.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
}

void appendUtf8(std::string& out, char32_t codePoint)
{
    if (codePoint < 0x80)
    {
        out.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

} // namespace

Result<std::wstring> utf8To16(std::string_view utf8)
{
    std::wstring out;
    out.reserve(utf8.size());
    for (std::size_t pos = 0; pos < utf8.size();)
    {
        const Decoded decoded = decodeUtf8(utf8, pos);
        if (!decoded.valid)
        {
            return fail(ErrorCode::InvalidData, std::format("invalid UTF-8 at byte {}", pos));
        }
        appendUtf16(out, decoded.codePoint);
        pos += decoded.length;
    }
    return out;
}

Result<std::string> utf16To8(std::wstring_view utf16)
{
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t pos = 0; pos < utf16.size();)
    {
        const Decoded decoded = decodeUtf16(utf16, pos);
        if (!decoded.valid)
        {
            return fail(ErrorCode::InvalidData, std::format("invalid UTF-16 at unit {}", pos));
        }
        appendUtf8(out, decoded.codePoint);
        pos += decoded.length;
    }
    return out;
}

std::wstring utf8To16Lossy(std::string_view utf8)
{
    std::wstring out;
    out.reserve(utf8.size());
    for (std::size_t pos = 0; pos < utf8.size();)
    {
        const Decoded decoded = decodeUtf8(utf8, pos);
        appendUtf16(out, decoded.codePoint);
        pos += decoded.length;
    }
    return out;
}

std::string utf16To8Lossy(std::wstring_view utf16)
{
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t pos = 0; pos < utf16.size();)
    {
        const Decoded decoded = decodeUtf16(utf16, pos);
        appendUtf8(out, decoded.codePoint);
        pos += decoded.length;
    }
    return out;
}

bool isValidUtf8(std::string_view text) noexcept
{
    for (std::size_t pos = 0; pos < text.size();)
    {
        const Decoded decoded = decodeUtf8(text, pos);
        if (!decoded.valid)
        {
            return false;
        }
        pos += decoded.length;
    }
    return true;
}

} // namespace rmw
