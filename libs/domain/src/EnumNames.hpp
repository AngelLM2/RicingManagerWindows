#pragma once

#include "rmw/domain/Rice.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// rice.toml spelling of every enum (internal to rmw::domain).
namespace rmw::domain::detail
{

template <class E>
struct EnumName
{
    E value;
    std::string_view name;
};

template <class E>
std::optional<E> enumFromName(std::span<const EnumName<E>> names, std::string_view text) noexcept
{
    for (const auto& entry : names)
    {
        if (entry.name == text)
        {
            return entry.value;
        }
    }
    return std::nullopt;
}

template <class E>
std::string_view enumToName(std::span<const EnumName<E>> names, E value) noexcept
{
    for (const auto& entry : names)
    {
        if (entry.value == value)
        {
            return entry.name;
        }
    }
    return "?";
}

// "light | dark | system" for error messages.
template <class E>
std::string enumChoices(std::span<const EnumName<E>> names)
{
    std::string text;
    for (const auto& entry : names)
    {
        if (!text.empty())
        {
            text += " | ";
        }
        text += entry.name;
    }
    return text;
}

inline constexpr auto kThemeModeNames = std::to_array<EnumName<ThemeMode>>({
    {ThemeMode::Light, "light"},
    {ThemeMode::Dark, "dark"},
    {ThemeMode::System, "system"},
    {ThemeMode::Schedule, "schedule"},
});

inline constexpr auto kWallpaperLayoutNames = std::to_array<EnumName<WallpaperLayout>>({
    {WallpaperLayout::PerMonitor, "per-monitor"},
    {WallpaperLayout::Span, "span"},
});

inline constexpr auto kWallpaperKindNames = std::to_array<EnumName<WallpaperKind>>({
    {WallpaperKind::Image, "image"},
    {WallpaperKind::Slideshow, "slideshow"},
    {WallpaperKind::Video, "video"},
    {WallpaperKind::Color, "color"},
});

inline constexpr auto kWallpaperFitNames = std::to_array<EnumName<WallpaperFit>>({
    {WallpaperFit::Fill, "fill"},
    {WallpaperFit::Fit, "fit"},
    {WallpaperFit::Stretch, "stretch"},
    {WallpaperFit::Center, "center"},
    {WallpaperFit::Tile, "tile"},
});

inline constexpr auto kParticleKindNames = std::to_array<EnumName<ParticleKind>>({
    {ParticleKind::Sparkles, "sparkles"},
    {ParticleKind::Stars, "stars"},
    {ParticleKind::Snow, "snow"},
    {ParticleKind::Rain, "rain"},
    {ParticleKind::Petals, "petals"},
    {ParticleKind::Fireflies, "fireflies"},
    {ParticleKind::Magic, "magic"},
    {ParticleKind::Notes, "notes"},
    {ParticleKind::Hearts, "hearts"},
    {ParticleKind::Bubbles, "bubbles"},
});

inline constexpr auto kTaskbarAlignmentNames = std::to_array<EnumName<TaskbarAlignment>>({
    {TaskbarAlignment::Left, "left"},
    {TaskbarAlignment::Center, "center"},
});

inline constexpr auto kTaskbarSearchNames = std::to_array<EnumName<TaskbarSearch>>({
    {TaskbarSearch::Hidden, "hidden"},
    {TaskbarSearch::Icon, "icon"},
    {TaskbarSearch::Box, "box"},
    {TaskbarSearch::Button, "button"},
});

inline constexpr auto kBarPositionNames = std::to_array<EnumName<BarPosition>>({
    {BarPosition::Top, "top"},
    {BarPosition::Bottom, "bottom"},
    {BarPosition::Left, "left"},
    {BarPosition::Right, "right"},
});

inline constexpr auto kStartMenuNames = std::to_array<EnumName<StartMenuKind>>({
    {StartMenuKind::Windows, "windows"},
    {StartMenuKind::Ours, "ours"},
});

inline constexpr auto kStartLayoutNames = std::to_array<EnumName<StartLayout>>({
    {StartLayout::Default, "default"},
    {StartLayout::MorePins, "more-pins"},
    {StartLayout::MoreRecommendations, "more-recommendations"},
});

inline constexpr auto kBarStyleNames = std::to_array<EnumName<BarStyle>>({
    {BarStyle::Windows, "windows"},
    {BarStyle::Dock, "dock"},
    {BarStyle::Island, "island"},
    {BarStyle::Status, "status"},
    {BarStyle::Side, "side"},
});

inline constexpr auto kBarModeNames = std::to_array<EnumName<BarMode>>({
    {BarMode::Reserve, "reserve"},
    {BarMode::Float, "float"},
});

inline constexpr auto kBarBackgroundNames = std::to_array<EnumName<BarBackground::Kind>>({
    {BarBackground::Kind::Solid, "solid"},
    {BarBackground::Kind::Transparent, "transparent"},
    {BarBackground::Kind::Acrylic, "acrylic"},
    {BarBackground::Kind::Mica, "mica"},
});

struct CursorRoleNames
{
    CursorRole role;
    std::string_view riceName;
    std::string_view registryName;
};

// Order matches the CursorRole enumerators.
inline constexpr std::array<CursorRoleNames, kCursorRoleCount> kCursorRoleNames = {{
    {CursorRole::Normal, "normal", "Arrow"},
    {CursorRole::Help, "help", "Help"},
    {CursorRole::Working, "working", "AppStarting"},
    {CursorRole::Busy, "busy", "Wait"},
    {CursorRole::Precision, "precision", "Crosshair"},
    {CursorRole::Text, "text", "IBeam"},
    {CursorRole::Handwriting, "handwriting", "NWPen"},
    {CursorRole::Unavailable, "unavailable", "No"},
    {CursorRole::Vertical, "vertical", "SizeNS"},
    {CursorRole::Horizontal, "horizontal", "SizeWE"},
    {CursorRole::Diagonal1, "diagonal1", "SizeNWSE"},
    {CursorRole::Diagonal2, "diagonal2", "SizeNESW"},
    {CursorRole::Move, "move", "SizeAll"},
    {CursorRole::Alternate, "alternate", "UpArrow"},
    {CursorRole::Link, "link", "Hand"},
    {CursorRole::Location, "location", "Pin"},
    {CursorRole::Person, "person", "Person"},
}};

struct SoundEventNames
{
    SoundEvent event;
    std::string_view riceName;
    std::string_view registryKey; // under HKCU\AppEvents\Schemes
};

// Order matches the SoundEvent enumerators.
inline constexpr std::array<SoundEventNames, kSoundEventCount> kSoundEventNames = {{
    {SoundEvent::Beep, "beep", R"(Apps\.Default\.Default)"},
    {SoundEvent::Asterisk, "asterisk", R"(Apps\.Default\SystemAsterisk)"},
    {SoundEvent::Exclamation, "exclamation", R"(Apps\.Default\SystemExclamation)"},
    {SoundEvent::Critical, "critical", R"(Apps\.Default\SystemHand)"},
    {SoundEvent::Question, "question", R"(Apps\.Default\SystemQuestion)"},
    {SoundEvent::Notification, "notification", R"(Apps\.Default\Notification.Default)"},
    {SoundEvent::Message, "message", R"(Apps\.Default\Notification.IM)"},
    {SoundEvent::Mail, "mail", R"(Apps\.Default\Notification.Mail)"},
    {SoundEvent::Reminder, "reminder", R"(Apps\.Default\Notification.Reminder)"},
    {SoundEvent::DeviceConnect, "device-connect", R"(Apps\.Default\DeviceConnect)"},
    {SoundEvent::DeviceDisconnect, "device-disconnect", R"(Apps\.Default\DeviceDisconnect)"},
    {SoundEvent::DeviceFail, "device-fail", R"(Apps\.Default\DeviceFail)"},
    {SoundEvent::LowBattery, "low-battery", R"(Apps\.Default\LowBatteryAlarm)"},
    {SoundEvent::CriticalBattery, "critical-battery", R"(Apps\.Default\CriticalBatteryAlarm)"},
    {SoundEvent::Unlock, "unlock", R"(Apps\.Default\WindowsUnlock)"},
    {SoundEvent::PrintComplete, "print-complete", R"(Apps\.Default\PrintComplete)"},
    {SoundEvent::EmptyRecycleBin, "empty-recycle-bin", R"(Apps\Explorer\EmptyRecycleBin)"},
    {SoundEvent::Navigating, "navigating", R"(Apps\Explorer\Navigating)"},
}};

inline constexpr auto kDesktopIconNames = std::to_array<EnumName<DesktopIcon>>({
    {DesktopIcon::ThisPc, "this-pc"},
    {DesktopIcon::RecycleBin, "recycle-bin"},
    {DesktopIcon::RecycleBinFull, "recycle-bin-full"},
    {DesktopIcon::Network, "network"},
    {DesktopIcon::UserFiles, "user-files"},
    {DesktopIcon::ControlPanel, "control-panel"},
});

inline constexpr auto kUserFolderNames = std::to_array<EnumName<UserFolder>>({
    {UserFolder::Desktop, "desktop"},
    {UserFolder::Documents, "documents"},
    {UserFolder::Downloads, "downloads"},
    {UserFolder::Pictures, "pictures"},
    {UserFolder::Music, "music"},
    {UserFolder::Videos, "videos"},
});

inline constexpr auto kSoundSchemeNames = std::to_array<EnumName<SoundScheme>>({
    {SoundScheme::None, "none"},
});

inline constexpr auto kMachineIconNames = std::to_array<EnumName<MachineIcon>>({
    {MachineIcon::Folder, "folder"},
    {MachineIcon::Drive, "drive"},
});

inline constexpr auto kWindowCornersNames = std::to_array<EnumName<WindowCorners>>({
    {WindowCorners::Round, "round"},
    {WindowCorners::Small, "small"},
    {WindowCorners::Square, "square"},
});

inline constexpr auto kWindowBackdropNames = std::to_array<EnumName<WindowBackdrop>>({
    {WindowBackdrop::None, "none"},
    {WindowBackdrop::Mica, "mica"},
    {WindowBackdrop::Acrylic, "acrylic"},
    {WindowBackdrop::Tabbed, "tabbed"},
});

} // namespace rmw::domain::detail
