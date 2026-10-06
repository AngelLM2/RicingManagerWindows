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

} // namespace rmw::domain::detail
