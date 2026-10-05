#pragma once

#include "rmw/domain/Color.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/SemVer.hpp"
#include "rmw/domain/Value.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// In-memory model of rice.toml (docs/03 §6). Every section is optional: a rice only manages what
// it mentions, and an absent field means "leave the system as it is". Paths are package-relative
// and already validated by PathRules.
namespace rmw::domain
{

inline constexpr int kRiceSchemaVersion = 1;

struct RiceMeta
{
    std::string id; // optional reverse-DNS id; the installer derives one from the name when empty
    std::string name;
    std::string author;
    std::string description;
    std::optional<SemVer> version;
    std::string license;
    std::optional<SemVer> minApp;
};

// "#RRGGBB" or a palette entry ("palette.base"). "palette.accent" always resolves, falling back
// to the active accent color.
struct ColorRef
{
    std::variant<Color, std::string> value; // Color, or the palette entry name

    bool isPalette() const noexcept { return std::holds_alternative<std::string>(value); }
    std::string toString() const;

    friend bool operator==(const ColorRef&, const ColorRef&) = default;
};

struct PaletteEntry
{
    std::string name;
    Color color;
};

// ---- appearance -------------------------------------------------------------------------------

enum class ThemeMode
{
    Light,
    Dark,
    System, // leave the light/dark choice to Windows
    Schedule,
};

struct TimeOfDay
{
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;

    static Result<TimeOfDay> parse(std::string_view text); // "HH:MM", 24 h
    friend auto operator<=>(const TimeOfDay&, const TimeOfDay&) = default;
};

struct AppearanceSchedule
{
    TimeOfDay lightAt{7, 0};
    TimeOfDay darkAt{19, 0};
};

struct AppearanceConfig
{
    std::optional<ThemeMode> mode;
    std::optional<Color> accent;
    std::optional<bool> transparency;
    std::optional<bool> accentOnTaskbar;
    std::optional<AppearanceSchedule> schedule;
};

// ---- wallpaper --------------------------------------------------------------------------------

enum class WallpaperLayout
{
    PerMonitor,
    Span, // one image across every monitor
};

enum class WallpaperKind
{
    Image,
    Slideshow,
    Video,
    Color,
};

enum class WallpaperFit
{
    Fill,
    Fit,
    Stretch,
    Center,
    Tile,
};

struct WallpaperPolicy
{
    std::optional<bool> pauseOnFullscreen;
    std::optional<bool> pauseOnBattery;
    std::optional<bool> pauseWhenCovered;
    std::optional<std::uint32_t> fpsLimit;
    std::optional<bool> mute;
};

struct MonitorWallpaper
{
    MonitorSelector match;
    WallpaperKind kind = WallpaperKind::Image;
    std::string source;         // image/video file, or slideshow folder; empty for Color
    std::optional<Color> color; // Kind::Color
    WallpaperFit fit = WallpaperFit::Fill;
    std::string fallbackImage; // Kind::Video: shown while paused or when video is unavailable
    std::optional<std::uint32_t> intervalSeconds; // Kind::Slideshow
    std::optional<bool> shuffle;                  // Kind::Slideshow
};

struct WallpaperConfig
{
    WallpaperLayout layout = WallpaperLayout::PerMonitor;
    std::optional<Color> background; // global Windows background color
    std::string spanSource;          // Layout::Span: the image
    WallpaperPolicy policy;
    std::vector<MonitorWallpaper> monitors; // Layout::PerMonitor
};

// ---- cursors ----------------------------------------------------------------------------------

// The 17 system cursor roles. Rice names are the ones Windows Settings shows; registry value
// names are under HKCU\Control Panel\Cursors.
enum class CursorRole
{
    Normal,      // Arrow
    Help,        // Help
    Working,     // AppStarting
    Busy,        // Wait
    Precision,   // Crosshair
    Text,        // IBeam
    Handwriting, // NWPen
    Unavailable, // No
    Vertical,    // SizeNS
    Horizontal,  // SizeWE
    Diagonal1,   // SizeNWSE
    Diagonal2,   // SizeNESW
    Move,        // SizeAll
    Alternate,   // UpArrow
    Link,        // Hand
    Location,    // Pin
    Person,      // Person
};

inline constexpr std::size_t kCursorRoleCount = 17;

std::string_view riceName(CursorRole role) noexcept;
std::string_view registryValueName(CursorRole role) noexcept;
std::optional<CursorRole> cursorRoleFromRiceName(std::string_view name) noexcept;

struct CursorAssignment
{
    CursorRole role;
    std::string file; // relative to CursorConfig::source
};

struct CursorConfig
{
    std::string source; // folder inside the package
    std::optional<std::uint32_t> size;
    std::vector<CursorAssignment> map; // empty: read install.inf from `source`
};

// ---- native taskbar (ADR-0003) -----------------------------------------------------------------

enum class TaskbarAlignment
{
    Left,
    Center,
};

enum class TaskbarSearch
{
    Hidden,
    Icon,
    Box,
    Button,
};

struct TaskbarConfig
{
    std::optional<TaskbarAlignment> alignment; // applied after an explicit Explorer restart
    std::optional<bool> autohide;
    std::optional<bool> taskView;
    std::optional<TaskbarSearch> search;
};

// ---- own bar ([[bar]]) --------------------------------------------------------------------------

enum class BarPosition
{
    Top,
    Bottom,
    Left,
    Right,
};

enum class BarMode
{
    Reserve, // AppBar: pushes maximized windows away
    Float,   // over the desktop, no reserved space
};

struct BarLength
{
    bool percent = true;
    double value = 100.0; // percent of the monitor edge, or logical pixels
};

struct BarBackground
{
    enum class Kind
    {
        Solid,
        Transparent,
        Acrylic,
        Mica,
    };

    Kind kind = Kind::Solid;
    std::optional<ColorRef> tint;
    std::optional<double> opacity; // 0..1
};

// One widget slot: a built-in ("clock") or a plugin ("io.github.fulano.now-playing"), with its
// own settings (validated later by the widget's schema).
struct BarItem
{
    std::string id;
    Value config;

    bool isPlugin() const noexcept;
};

struct BarConfig
{
    MonitorSelector monitor;
    BarPosition position = BarPosition::Bottom;
    BarMode mode = BarMode::Reserve;
    std::uint32_t thickness = 40; // logical px
    BarLength length;
    std::array<std::uint32_t, 4> margin{}; // top, right, bottom, left
    std::uint32_t radius = 0;
    BarBackground background;
    std::optional<ColorRef> foreground;
    bool autohide = false;
    bool hideNativeTaskbar = false;
    std::vector<BarItem> left;
    std::vector<BarItem> center;
    std::vector<BarItem> right;
};

// ---- plugins ----------------------------------------------------------------------------------

struct PluginRequirement
{
    std::string id;
    VersionReq version;
};

struct PluginSettings
{
    std::string id;
    Value config;
};

// ---- the rice ---------------------------------------------------------------------------------

struct Rice
{
    int schema = kRiceSchemaVersion;
    RiceMeta meta;
    std::vector<PaletteEntry> palette;
    std::optional<AppearanceConfig> appearance;
    std::optional<WallpaperConfig> wallpaper;
    std::optional<CursorConfig> cursors;
    std::optional<TaskbarConfig> taskbar;
    std::vector<BarConfig> bars;
    std::vector<PluginRequirement> pluginRequirements;
    std::vector<PluginSettings> pluginSettings;

    const PaletteEntry* findPalette(std::string_view name) const noexcept;
};

// Enum <-> rice.toml spelling, shared by the parser, the exporter and error messages.
std::string_view toString(ThemeMode value) noexcept;
std::string_view toString(WallpaperLayout value) noexcept;
std::string_view toString(WallpaperKind value) noexcept;
std::string_view toString(WallpaperFit value) noexcept;
std::string_view toString(TaskbarAlignment value) noexcept;
std::string_view toString(TaskbarSearch value) noexcept;
std::string_view toString(BarPosition value) noexcept;
std::string_view toString(BarMode value) noexcept;
std::string_view toString(BarBackground::Kind value) noexcept;

} // namespace rmw::domain
