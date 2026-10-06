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

std::string_view riceName(SoundEvent event) noexcept
{
    return kSoundEventNames[static_cast<std::size_t>(event)].riceName;
}

std::string_view registryKey(SoundEvent event) noexcept
{
    return kSoundEventNames[static_cast<std::size_t>(event)].registryKey;
}

std::optional<SoundEvent> soundEventFromRiceName(std::string_view name) noexcept
{
    for (const auto& entry : kSoundEventNames)
    {
        if (entry.riceName == name)
        {
            return entry.event;
        }
    }
    return std::nullopt;
}

std::string_view riceName(DesktopIcon icon) noexcept
{
    return enumToName<DesktopIcon>(kDesktopIconNames, icon);
}

std::string_view riceName(UserFolder folder) noexcept
{
    return enumToName<UserFolder>(kUserFolderNames, folder);
}

std::optional<DesktopIcon> desktopIconFromRiceName(std::string_view name) noexcept
{
    return enumFromName<DesktopIcon>(kDesktopIconNames, name);
}

std::optional<UserFolder> userFolderFromRiceName(std::string_view name) noexcept
{
    return enumFromName<UserFolder>(kUserFolderNames, name);
}

bool isValidFileExtension(std::string_view extension) noexcept
{
    return !extension.empty() && extension.size() <= 16 && std::ranges::all_of(extension, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
    });
}

bool isValidProgramName(std::string_view name) noexcept
{
    if (name.size() < 5 || name.size() > 64)
    {
        return false;
    }
    const std::string_view extension = name.substr(name.size() - 4);
    const bool exe = (extension[0] == '.') && (extension[1] == 'e' || extension[1] == 'E') &&
                     (extension[2] == 'x' || extension[2] == 'X') && (extension[3] == 'e' || extension[3] == 'E');
    return exe && name[0] != '.' && name[0] != ' ' && std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '_' || c == '-' || c == ' ';
           });
}

namespace
{

std::vector<BarItem> items(std::initializer_list<std::string_view> ids)
{
    std::vector<BarItem> out;
    for (const std::string_view id : ids)
    {
        out.push_back({std::string(id), {}});
    }
    return out;
}

} // namespace

BarConfig barStyle(BarStyle style)
{
    BarConfig bar;
    bar.keys = {"position", "mode",       "thickness", "length",          "margin", "radius",
                "background", "autohide", "hide_native_taskbar", "left",  "center", "right"};
    bar.hideNativeTaskbar = true;
    bar.autohide = false;
    switch (style)
    {
    case BarStyle::Windows:
        bar.position = BarPosition::Bottom;
        bar.mode = BarMode::Reserve;
        bar.thickness = 48;
        bar.length = {true, 100.0, false};
        bar.margin = {0, 0, 0, 0};
        bar.radius = 0;
        bar.background = {BarBackground::Kind::Acrylic, std::nullopt, 0.85};
        bar.left = {};
        bar.center = items({"start", "windows"});
        bar.right = items({"tray", "network", "volume", "battery", "clock"});
        break;
    case BarStyle::Dock:
        // Like macOS' Dock: maximized windows end above it (v1.7: they went under it). Float is a choice.
        bar.position = BarPosition::Bottom;
        bar.mode = BarMode::Reserve;
        bar.thickness = 60;
        bar.length = {true, 100.0, true};
        bar.margin = {0, 0, 10, 0};
        bar.radius = 20;
        bar.background = {BarBackground::Kind::Acrylic, std::nullopt, 0.7};
        bar.left = {};
        bar.center = items({"start", "dock", "separator", "tray", "clock"});
        bar.right = {};
        break;
    case BarStyle::Island:
        bar.position = BarPosition::Top;
        bar.mode = BarMode::Reserve;
        bar.thickness = 36;
        bar.length = {true, 100.0, true};
        bar.margin = {8, 0, 0, 0};
        bar.radius = 18;
        bar.background = {BarBackground::Kind::Acrylic, std::nullopt, 0.8};
        bar.left = {};
        bar.center = items({"start", "windows", "separator", "media", "volume", "network", "battery", "clock"});
        bar.right = {};
        break;
    case BarStyle::Status:
        bar.position = BarPosition::Top;
        bar.mode = BarMode::Reserve;
        bar.thickness = 28;
        bar.length = {true, 100.0, false};
        bar.margin = {0, 0, 0, 0};
        bar.radius = 0;
        bar.background = {BarBackground::Kind::Solid, std::nullopt, 0.95};
        bar.left = items({"start", "desktops", "windows"});
        bar.center = items({"clock"});
        bar.right = items({"cpu", "memory", "network", "volume", "battery", "tray"});
        break;
    case BarStyle::Side:
        bar.position = BarPosition::Left;
        bar.mode = BarMode::Reserve;
        bar.thickness = 52;
        bar.length = {true, 100.0, false};
        bar.margin = {0, 0, 0, 0};
        bar.radius = 0;
        bar.background = {BarBackground::Kind::Acrylic, std::nullopt, 0.85};
        bar.left = items({"start", "desktops", "windows"});
        bar.center = {};
        bar.right = items({"tray", "volume", "network", "battery", "clock"});
        break;
    }
    return bar;
}

BarConfig withStyle(const BarConfig& bar)
{
    return bar.style ? applySkin(bar, barStyle(*bar.style)) : bar;
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

std::string_view toString(StartMenuKind value) noexcept
{
    return enumToName<StartMenuKind>(kStartMenuNames, value);
}

std::string_view toString(StartLayout value) noexcept
{
    return enumToName<StartLayout>(kStartLayoutNames, value);
}

std::string_view toString(BarStyle value) noexcept
{
    return enumToName<BarStyle>(kBarStyleNames, value);
}

std::string_view toString(BarMode value) noexcept
{
    return enumToName<BarMode>(kBarModeNames, value);
}

std::string_view toString(BarBackground::Kind value) noexcept
{
    return enumToName<BarBackground::Kind>(kBarBackgroundNames, value);
}

std::string_view toString(SoundScheme value) noexcept
{
    return enumToName<SoundScheme>(kSoundSchemeNames, value);
}

std::string_view riceName(MachineIcon icon) noexcept
{
    return enumToName<MachineIcon>(kMachineIconNames, icon);
}

std::optional<MachineIcon> machineIconFromRiceName(std::string_view name) noexcept
{
    return enumFromName<MachineIcon>(kMachineIconNames, name);
}

std::string_view toString(WindowCorners value) noexcept
{
    return enumToName<WindowCorners>(kWindowCornersNames, value);
}

std::string_view toString(WindowBackdrop value) noexcept
{
    return enumToName<WindowBackdrop>(kWindowBackdropNames, value);
}

std::optional<WindowCorners> windowCornersFromName(std::string_view name) noexcept
{
    return enumFromName<WindowCorners>(kWindowCornersNames, name);
}

std::optional<WindowBackdrop> windowBackdropFromName(std::string_view name) noexcept
{
    return enumFromName<WindowBackdrop>(kWindowBackdropNames, name);
}

} // namespace rmw::domain
