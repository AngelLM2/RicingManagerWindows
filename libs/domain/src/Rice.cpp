#include "rmw/domain/Rice.hpp"

#include "EnumNames.hpp"

#include <algorithm>
#include <format>

namespace rmw::domain
{

using namespace detail;

std::string ColorRef::toString() const
{
    if (const auto* name = std::get_if<std::string>(&value))
    {
        return "palette." + *name;
    }
    return std::get<Color>(value).toHex();
}

Result<TimeOfDay> TimeOfDay::parse(std::string_view text)
{
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (text.size() == 5 && digit(text[0]) && digit(text[1]) && text[2] == ':' && digit(text[3]) &&
        digit(text[4]))
    {
        const int hour = (text[0] - '0') * 10 + (text[1] - '0');
        const int minute = (text[3] - '0') * 10 + (text[4] - '0');
        if (hour < 24 && minute < 60)
        {
            return TimeOfDay{static_cast<std::uint8_t>(hour), static_cast<std::uint8_t>(minute)};
        }
    }
    return fail(ErrorCode::InvalidData, std::format("invalid time '{}' (expected HH:MM, 24-hour)", text));
}

std::string_view riceName(CursorRole role) noexcept
{
    return kCursorRoleNames[static_cast<std::size_t>(role)].riceName;
}

std::string_view registryValueName(CursorRole role) noexcept
{
    return kCursorRoleNames[static_cast<std::size_t>(role)].registryName;
}

std::optional<CursorRole> cursorRoleFromRiceName(std::string_view name) noexcept
{
    for (const auto& entry : kCursorRoleNames)
    {
        if (entry.riceName == name)
        {
            return entry.role;
        }
    }
    return std::nullopt;
}

bool WallpaperEffects::empty() const noexcept
{
    return !brightness && !contrast && !saturation && !hue && !speed && !blur && !parallax && !zoom &&
           !vignette;
}

bool BarItem::isPlugin() const noexcept
{
    // Built-in widgets never contain a dot; plugin ids are reverse-DNS.
    return id.find('.') != std::string::npos;
}

BarConfig applySkin(const BarConfig& bar, const BarConfig& skin)
{
    BarConfig merged = bar;
    for (const std::string& key : skin.keys)
    {
        if (std::ranges::find(bar.keys, key) != bar.keys.end())
        {
            continue; // the rice's own choice wins
        }
        if (key == "position")
        {
            merged.position = skin.position;
        }
        else if (key == "mode")
        {
            merged.mode = skin.mode;
        }
        else if (key == "thickness")
        {
            merged.thickness = skin.thickness;
        }
        else if (key == "length")
        {
            merged.length = skin.length;
        }
        else if (key == "margin")
        {
            merged.margin = skin.margin;
        }
        else if (key == "radius")
        {
            merged.radius = skin.radius;
        }
        else if (key == "background")
        {
            merged.background = skin.background;
        }
        else if (key == "foreground")
        {
            merged.foreground = skin.foreground;
        }
        else if (key == "autohide")
        {
            merged.autohide = skin.autohide;
        }
        else if (key == "hide_native_taskbar")
        {
            merged.hideNativeTaskbar = skin.hideNativeTaskbar;
        }
        else if (key == "left")
        {
            merged.left = skin.left;
        }
        else if (key == "center")
        {
            merged.center = skin.center;
        }
        else if (key == "right")
        {
            merged.right = skin.right;
        }
        else
        {
            continue;
        }
        merged.keys.push_back(key);
    }
    return merged;
}

const PaletteEntry* Rice::findPalette(std::string_view name) const noexcept
{
    for (const auto& entry : palette)
    {
        if (entry.name == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

std::string_view toString(ThemeMode value) noexcept
{
    return enumToName<ThemeMode>(kThemeModeNames, value);
}

std::string_view toString(WallpaperLayout value) noexcept
{
    return enumToName<WallpaperLayout>(kWallpaperLayoutNames, value);
}

std::string_view toString(WallpaperKind value) noexcept
{
    return enumToName<WallpaperKind>(kWallpaperKindNames, value);
}

std::string_view toString(WallpaperFit value) noexcept
{
    return enumToName<WallpaperFit>(kWallpaperFitNames, value);
}

std::string_view toString(ParticleKind value) noexcept
{
    return enumToName<ParticleKind>(kParticleKindNames, value);
}

std::string_view toString(TaskbarAlignment value) noexcept
{
    return enumToName<TaskbarAlignment>(kTaskbarAlignmentNames, value);
}

std::string_view toString(TaskbarSearch value) noexcept
{
    return enumToName<TaskbarSearch>(kTaskbarSearchNames, value);
}

std::string_view toString(BarPosition value) noexcept
{
    return enumToName<BarPosition>(kBarPositionNames, value);
}

std::string_view toString(BarMode value) noexcept
{
    return enumToName<BarMode>(kBarModeNames, value);
}

std::string_view toString(BarBackground::Kind value) noexcept
{
    return enumToName<BarBackground::Kind>(kBarBackgroundNames, value);
}

} // namespace rmw::domain
