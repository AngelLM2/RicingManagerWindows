#include "rmw/domain/Rice.hpp"

#include "EnumNames.hpp"

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

bool BarItem::isPlugin() const noexcept
{
    // Built-in widgets never contain a dot; plugin ids are reverse-DNS.
    return id.find('.') != std::string::npos;
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
