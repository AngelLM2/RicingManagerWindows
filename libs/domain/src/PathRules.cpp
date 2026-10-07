#include "rmw/domain/PathRules.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"

#include <array>
#include <format>
#include <vector>

namespace rmw::domain
{
namespace
{

constexpr std::size_t kSafeNameMaxBytes = 60;
constexpr std::size_t kKeptExtensionMaxBytes = 10; // ".jpeg", ".webm": what a long name keeps

char asciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (asciiLower(a[i]) != asciiLower(b[i]))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool isReservedDeviceName(std::string_view component) noexcept
{
    // The device name applies to the part before the first dot: "con.txt" is reserved too.
    const std::string_view base = component.substr(0, component.find('.'));
    static constexpr std::array<std::string_view, 4> fixed = {"con", "prn", "aux", "nul"};
    for (const auto name : fixed)
    {
        if (equalsIgnoreCase(base, name))
        {
            return true;
        }
    }
    for (const std::string_view prefix : {std::string_view("com"), std::string_view("lpt")})
    {
        if (base.size() < 4 || !equalsIgnoreCase(base.substr(0, 3), prefix))
        {
            continue;
        }
        const std::string_view suffix = base.substr(3);
        if (suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9')
        {
            return true;
        }
        // Superscript one, two, three in UTF-8.
        if (suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3")
        {
            return true;
        }
    }
    return false;
}

namespace
{

bool looksLikeShortName(std::string_view component) noexcept
{
    for (std::size_t i = 0; i + 1 < component.size(); ++i)
    {
        if (component[i] == '~' && component[i + 1] >= '0' && component[i + 1] <= '9')
        {
            return true;
        }
    }
    return false;
}

std::string checkComponent(std::string_view component)
{
    if (component.empty())
    {
        return "empty path component";
    }
    if (component == "." || component == "..")
    {
        return "'.' and '..' are not allowed";
    }
    if (component.size() > limits::kPathComponentMaxChars)
    {
        return "path component too long";
    }
    for (const char c : component)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*')
        {
            return "forbidden character in path";
        }
    }
    if (component.back() == '.' || component.back() == ' ')
    {
        return "path component ends with '.' or space";
    }
    if (isReservedDeviceName(component))
    {
        return "reserved device name";
    }
    if (looksLikeShortName(component))
    {
        return "8.3 short-name alias ('~' followed by a digit)";
    }
    return {};
}

// At most `maxBytes`, never half a character.
void cutAtCharacter(std::string& text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
    {
        return;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    text.resize(cut);
}

void trimEnd(std::string& text)
{
    while (!text.empty() && (text.back() == '.' || text.back() == ' '))
    {
        text.pop_back();
    }
}

// ".jpg" of "city.jpg": a short run of ASCII letters and digits after the last dot ("" when none).
std::string_view extensionOf(std::string_view name) noexcept
{
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || name.size() - dot - 1 == 0 ||
        name.size() - dot > kKeptExtensionMaxBytes)
    {
        return {};
    }
    for (const char c : name.substr(dot + 1))
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
        {
            return {};
        }
    }
    return name.substr(dot);
}

} // namespace

Result<std::string> validatePackagePath(std::string_view path)
{
    const auto reject = [&](std::string_view why) {
        return fail(ErrorCode::InvalidData, std::format("invalid package path '{}': {}", path, why));
    };

    if (path.empty())
    {
        return reject("empty");
    }
    if (path.size() > limits::kRelativePathMaxChars)
    {
        return reject("too long");
    }
    if (!isValidUtf8(path))
    {
        return reject("not valid UTF-8");
    }

    std::string normalized(path);
    for (char& c : normalized)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    if (normalized.front() == '/')
    {
        return reject("absolute paths are not allowed");
    }

    std::size_t start = 0;
    while (start <= normalized.size())
    {
        const std::size_t end = normalized.find('/', start);
        const std::string_view component =
            std::string_view(normalized)
                .substr(start, (end == std::string::npos ? normalized.size() : end) - start);
        if (const std::string why = checkComponent(component); !why.empty())
        {
            return reject(why);
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return normalized;
}

std::string safeFileName(std::string_view name, std::string_view fallback)
{
    std::string out;
    for (const char c : name)
    {
        const auto byte = static_cast<unsigned char>(c);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '-' || c == '_' || byte >= 0x80;
        out += ok ? c : '-';
    }
    // A long name loses the end of its stem, never its extension: "<long>.jpg" cut to 60 bytes
    // lost ".jpg", and the rice that named it no longer read.
    if (out.size() > kSafeNameMaxBytes)
    {
        const std::string extension(extensionOf(out));
        std::string stem = out.substr(0, out.size() - extension.size());
        cutAtCharacter(stem, kSafeNameMaxBytes - extension.size());
        trimEnd(stem);
        out = stem.empty() ? std::string() : stem + extension;
    }
    trimEnd(out);
    while (!out.empty() && out.front() == ' ')
    {
        out.erase(out.begin());
    }
    if (out.empty() || !validatePackagePath(out))
    {
        return std::string(fallback);
    }
    return out;
}

bool hasExtension(std::string_view path, std::initializer_list<std::string_view> allowed) noexcept
{
    const auto dot = path.rfind('.');
    const auto slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
    {
        return false;
    }
    const std::string_view extension = path.substr(dot + 1);
    for (const auto candidate : allowed)
    {
        if (equalsIgnoreCase(extension, candidate))
        {
            return true;
        }
    }
    return false;
}

} // namespace rmw::domain
