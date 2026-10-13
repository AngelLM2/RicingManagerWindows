#include "rmw/domain/Tweaks.hpp"

#include "TomlReader.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <vector>

namespace rmw::domain
{
namespace
{

constexpr std::size_t kKeyMaxBytes = 255;
constexpr std::size_t kKeyMaxComponents = 16;
constexpr std::size_t kValueNameMaxBytes = 128;
constexpr std::size_t kStringMaxBytes = 260;

constexpr auto kAllowedKeys = std::to_array<std::string_view>({
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)",
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons)",
    R"(Software\Microsoft\Windows\CurrentVersion\Search)",
    R"(Software\Microsoft\Windows\CurrentVersion\Feeds)",
    R"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)",
    R"(Software\Microsoft\Windows\CurrentVersion\AdvertisingInfo)",
    R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
    R"(Control Panel\Desktop)",
    R"(Control Panel\Colors)",
    R"(Control Panel\Mouse)",
    R"(Control Panel\Keyboard)",
    R"(AppEvents\Schemes)",
});

// Wins over the allow-list, at any depth: keys that start programs, load code, or change where
// Windows looks for things (docs/06 §6).
constexpr auto kBlockedComponents = std::to_array<std::string_view>({
    "Run",
    "RunOnce",
    "RunOnceEx",
    "RunServices",
    "RunServicesOnce",
    "StartupApproved",
    "Startup",
    "Classes",
    "CLSID",
    "Policies",
    "Environment",
    "Volatile Environment",
    "Winlogon",
    "Windows NT",
    "Image File Execution Options",
    "SilentProcessExit",
    "AppCompatFlags",
    "App Paths",
    "Shell Extensions",
    "ShellServiceObjectDelayLoad",
    "ShellExecuteHooks",
    "ShellIconOverlayIdentifiers",
    "Browser Helper Objects",
    "Command Processor",
    "Terminal Server",
    "Services",
    "Scripts",
    "Shell Folders",
    "User Shell Folders",
    "FileExts",
    "Internet Settings",
    "Protocols",
});

// Values that start programs wherever they are.
constexpr auto kBlockedValueNames = std::to_array<std::string_view>({
    "Shell",
    "Userinit",
    "Load",
    "Run",
    "Debugger",
    "AppInit_DLLs",
    "SCRNSAVE.EXE",
    "AutoRun",
});

struct OwnedValue
{
    std::string_view key;
    std::string_view name; // a trailing '*' matches any name with that start
    std::string_view owner;
    bool subtree = false; // every value of the key and of the keys below it
};

// Values our modules manage (a tweak would fight them, and drift detection would undo it).
constexpr auto kOwnedValues = std::to_array<OwnedValue>({
    {R"(Control Panel\Desktop)", "Wallpaper*", "the wallpaper"},
    {R"(Control Panel\Desktop)", "TileWallpaper", "the wallpaper"},
    {R"(Control Panel\Desktop)", "Transcoded*", "the wallpaper"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", "AppsUseLightTheme",
     "light/dark mode"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", "SystemUsesLightTheme",
     "light/dark mode"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", "EnableTransparency",
     "the appearance"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", "ColorPrevalence", "the appearance"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "TaskbarAl", "the taskbar"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "ShowTaskViewButton", "the taskbar"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Search)", "SearchboxTaskbarMode", "the taskbar"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "Start_Layout", "the Start menu"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "Start_TrackDocs", "the Start menu"},
    {R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "Start_IrisRecommendations",
     "the Start menu"},
    {R"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)", "RotatingLockScreen*",
     "the lock screen"},
    {R"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)", "SubscribedContent-338387Enabled",
     "the lock screen"},
    // v1.9: the sounds module holds every event (a rice's "no sounds" silences them all).
    {R"(AppEvents\Schemes)", "*", "the sounds", true},
});

// File types that run or load code. Any ".exe" inside the data is refused, wherever it is.
constexpr auto kCodeExtensions = std::to_array<std::string_view>({
    "exe",        "dll",       "com",         "bat",       "cmd",    "scr",
    "pif",        "cpl",       "msc",         "msi",       "msp",    "mst",
    "ps1",        "psm1",      "psd1",        "vbs",       "vbe",    "js",
    "jse",        "wsf",       "wsh",         "wsc",       "hta",    "lnk",
    "url",        "reg",       "sys",         "ocx",       "drv",    "jar",
    "inf",        "scf",       "application", "appref-ms", "gadget", "settingcontent-ms",
    "library-ms", "search-ms", "diagcab",     "msix",      "appx",   "appinstaller",
    "xll",        "chm",
});

// Programs that run other code ("rundll32 x,Entry" needs no ".exe").
constexpr auto kLaunchers = std::to_array<std::string_view>({
    "powershell",
    "pwsh",
    "cmd",
    "wscript",
    "cscript",
    "mshta",
    "rundll32",
    "regsvr32",
    "msiexec",
    "certutil",
    "bitsadmin",
    "schtasks",
    "conhost",
    "wmic",
    "forfiles",
});

char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, {}, lower, lower);
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && equalsIgnoringCase(text.substr(0, prefix.size()), prefix);
}

// Registry names compare case-insensitively with Unicode rules we do not reproduce; keeping key
// names to a plain ASCII set makes the block-list exact.
bool isKeyChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view(" _-.{}()&,'+").find(c) != std::string_view::npos;
}

std::vector<std::string_view> split(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = path.find('\\', start);
        parts.push_back(path.substr(start, (slash == std::string_view::npos ? path.size() : slash) - start));
        if (slash == std::string_view::npos)
        {
            return parts;
        }
        start = slash + 1;
    }
}

} // namespace

Result<std::string> validateTweakKey(std::string_view key)
{
    std::string path(key);
    std::ranges::replace(path, '/', '\\');
    std::string_view rest(path);
    for (const std::string_view hive :
         {std::string_view(R"(HKEY_CURRENT_USER\)"), std::string_view(R"(HKCU\)")})
    {
        if (startsWithIgnoringCase(rest, hive))
        {
            rest.remove_prefix(hive.size());
            break;
        }
    }
    if (startsWithIgnoringCase(rest, "HK") || rest.find(':') != std::string_view::npos)
    {
        return fail(ErrorCode::InvalidData, "a tweak writes only under HKEY_CURRENT_USER");
    }
    if (rest.empty() || rest.size() > kKeyMaxBytes)
    {
        return fail(ErrorCode::InvalidData, "the key is empty or too long");
    }
    const auto parts = split(rest);
    if (parts.size() > kKeyMaxComponents)
    {
        return fail(ErrorCode::InvalidData, "the key is too deep");
    }
    for (const std::string_view part : parts)
    {
        if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ' ||
            !std::ranges::all_of(part, isKeyChar))
        {
            return fail(
                ErrorCode::InvalidData,
                std::format("invalid key component '{}' (letters, digits, spaces and _-.{{}}()&,'+)", part));
        }
        for (const std::string_view blocked : kBlockedComponents)
        {
            if (equalsIgnoringCase(part, blocked))
            {
                return fail(ErrorCode::AccessDenied,
                            std::format("tweaks can never write under '{}' (it can start programs or "
                                        "change how Windows finds them)",
                                        part));
            }
        }
    }
    for (const std::string_view allowed : kAllowedKeys)
    {
        const auto allowedParts = split(allowed);
        if (parts.size() < allowedParts.size())
        {
            continue;
        }
        if (std::ranges::equal(allowedParts, parts | std::views::take(allowedParts.size()),
                               equalsIgnoringCase))
        {
            std::string canonical(allowed);
            for (std::size_t i = allowedParts.size(); i < parts.size(); ++i)
            {
                canonical += '\\';
                canonical += parts[i];
            }
            return canonical;
        }
    }
    return fail(
        ErrorCode::AccessDenied,
        std::format("'{}' is not a key tweaks may change (see the allowed keys in docs/plugins)", rest));
}

Result<void> validateTweakValueName(std::string_view key, std::string_view name)
{
    if (name.size() > kValueNameMaxBytes ||
        !std::ranges::all_of(name, [](char c) { return c >= 0x20 && c < 0x7f && c != '\\'; }))
    {
        return fail(ErrorCode::InvalidData,
                    "the value name is too long or has characters other than printable ASCII");
    }
    for (const std::string_view blocked : kBlockedValueNames)
    {
        if (equalsIgnoringCase(name, blocked))
        {
            return fail(ErrorCode::AccessDenied,
                        std::format("tweaks can never write the value '{}' (it starts programs)", name));
        }
    }
    for (const auto& owned : kOwnedValues)
    {
        const bool below = owned.subtree && key.size() > owned.key.size() && key[owned.key.size()] == '\\' &&
                           startsWithIgnoringCase(key, owned.key);
        if (!below && !equalsIgnoringCase(key, owned.key))
        {
            continue;
        }
        const bool matches = owned.name.ends_with('*')
                                 ? startsWithIgnoringCase(name, owned.name.substr(0, owned.name.size() - 1))
                                 : equalsIgnoringCase(name, owned.name);
        if (matches)
        {
            return fail(ErrorCode::AccessDenied,
                        std::format("'{}' belongs to {}: set it in the rice instead", name, owned.owner));
        }
    }
    return {};
}

Result<void> validateTweakString(std::string_view data)
{
    if (data.size() > kStringMaxBytes || !detail::isDisplaySafeText(data, false))
    {
        return fail(ErrorCode::InvalidData, "the text is too long or has control characters");
    }
    if (data.find('%') != std::string_view::npos)
    {
        return fail(ErrorCode::AccessDenied, "environment variables (%...%) are not allowed in tweak data");
    }
    if (data.find(R"(\\)") != std::string_view::npos || data.find("//") != std::string_view::npos)
    {
        return fail(ErrorCode::AccessDenied, "network paths and URLs are not allowed in tweak data");
    }
    std::string text(data);
    std::ranges::transform(text, text.begin(), lower);
    const auto isWordChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    };
    for (std::size_t dot = text.find('.'); dot != std::string::npos; dot = text.find('.', dot + 1))
    {
        std::size_t end = dot + 1;
        while (end < text.size() && isWordChar(text[end]))
        {
            ++end;
        }
        const std::string_view extension = std::string_view(text).substr(dot + 1, end - dot - 1);
        if (std::ranges::find(kCodeExtensions, extension) != kCodeExtensions.end())
        {
            return fail(ErrorCode::AccessDenied,
                        std::format("tweak data cannot name a program, script or library (.{})", extension));
        }
    }
    for (std::size_t start = 0; start < text.size();)
    {
        while (start < text.size() && !isWordChar(text[start]))
        {
            ++start;
        }
        std::size_t end = start;
        while (end < text.size() && isWordChar(text[end]) && text[end] != '-')
        {
            ++end;
        }
        const std::string_view word = std::string_view(text).substr(start, end - start);
        if (std::ranges::find(kLaunchers, word) != kLaunchers.end())
        {
            return fail(ErrorCode::AccessDenied,
                        std::format("tweak data cannot start programs ('{}')", word));
        }
        start = end == start ? end + 1 : end;
    }
    return {};
}

std::span<const std::string_view> tweakAllowedKeys() noexcept
{
    return kAllowedKeys;
}

} // namespace rmw::domain
