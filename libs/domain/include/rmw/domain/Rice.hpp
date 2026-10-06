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
    // false: the rice holds files that are not its author's to give (a Wallpaper Engine wallpaper):
    // it is never packed or shared, only used on this computer.
    std::optional<bool> shareable;

    [[nodiscard]] bool canShare() const noexcept { return shareable.value_or(true); }
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

// What rmw-wallpaper does to a picture, a video or a color (M12): colors, motion. Every value is
// optional: what the rice leaves out stays as it is (and is not written back).
struct WallpaperEffects
{
    std::optional<double> brightness; // -1..1, 0 = as it is
    std::optional<double> contrast;   // 0..2, 1 = as it is
    std::optional<double> saturation; // 0..2, 1 = as it is (0 = gray)
    std::optional<double> hue;        // -180..180 degrees
    std::optional<double> speed;      // a video's playback rate, 0.25..4
    std::optional<double> blur;       // 0..40 (logical pixels; pictures)
    std::optional<double> parallax;   // 0..1: the picture follows the mouse a little
    std::optional<double> zoom;       // 0..1: a slow pan and zoom (pictures)
    std::optional<double> vignette;   // 0..1: darker corners

    [[nodiscard]] bool empty() const noexcept;
    friend bool operator==(const WallpaperEffects&, const WallpaperEffects&) = default;
};

enum class ParticleKind
{
    Sparkles,  // four-pointed glints that twinkle
    Stars,     // small dots that twinkle, still
    Snow,      // flakes falling, swaying
    Rain,      // streaks falling, slanted
    Petals,    // petals falling, turning
    Fireflies, // glowing dots wandering
    Magic,     // glowing motes rising, with a trail
    Notes,     // musical notes floating up
    Hearts,    // hearts floating up
    Bubbles,   // rings rising
};

// A layer of particles drawn over the wallpaper (M12).
struct ParticleLayer
{
    ParticleKind kind = ParticleKind::Sparkles;
    std::optional<ColorRef> color;      // default: the kind's own
    std::optional<std::uint32_t> count; // 1..1000
    std::optional<double> size;         // 0.25..4 (1 = the kind's own)
    std::optional<double> speed;        // 0..4 (1 = the kind's own)
    std::optional<double> direction;    // -360..360 degrees, 90 = down, 270 = up
    std::optional<bool> glow;           // a soft halo

    friend bool operator==(const ParticleLayer&, const ParticleLayer&) = default;
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
    // Image, Video and Color (M12): with either, rmw-wallpaper draws this monitor; Windows' own
    // wallpaper stays underneath (like a video's fallback).
    WallpaperEffects effects;
    std::vector<ParticleLayer> particles;

    // Drawn by rmw-wallpaper: a video, or a picture or color with effects or particles.
    [[nodiscard]] bool live() const noexcept
    {
        return kind == WallpaperKind::Video || !effects.empty() || !particles.empty();
    }
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

// ---- Start ([start], M11) ----------------------------------------------------------------------

enum class StartMenuKind
{
    Windows, // Windows' own Start (what the rice says below about it still applies)
    Ours,    // Ricing Manager's: opened by our bar's Start button (and the Win key, if the user wants)
};

// Windows' own Start: what its settings allow (Settings > Personalization > Start).
enum class StartLayout
{
    Default,
    MorePins,
    MoreRecommendations,
};

struct StartConfig
{
    std::optional<StartMenuKind> menu;
    std::vector<std::string> pinned;       // our menu's pinned apps, ids as `rmw apps` lists them
    std::optional<StartLayout> layout;     // Windows' menu
    std::optional<bool> recommendations;   // Windows' menu: recent files and tips in "Recommended"
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
    bool fit = false;     // "auto": as long as what the bar shows, centered (M10)
};

// A ready-made look for a bar (M10): like a skin that comes with Ricing Manager. The bar's own keys
// win, then its skin's, then the style's.
enum class BarStyle
{
    Windows, // at the bottom, Start and the apps in the middle, like Windows 11
    Dock,    // floating at the bottom, as long as its apps, icons that grow under the mouse
    Island,  // a floating pill at the top
    Status,  // a thin line at the top, Waybar/Polybar style
    Side,    // along the left edge
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
    // A bar.skin plugin this bar starts from: the skin fills every key the bar leaves out.
    std::string skin;
    std::optional<BarStyle> style; // M10: fills what the bar and its skin leave out
    // The keys written in rice.toml (or in the skin's [skin]): "thickness", "left"...
    std::vector<std::string> keys;
};

// The bar with what `skin` (a plugin's [skin]) writes and `bar` leaves out.
[[nodiscard]] BarConfig applySkin(const BarConfig& bar, const BarConfig& skin);
// A style as a skin: every key it sets is in `keys`.
[[nodiscard]] BarConfig barStyle(BarStyle style);
// The bar with its style applied (after its skin, which the core applies first).
[[nodiscard]] BarConfig withStyle(const BarConfig& bar);

// ---- plugins ----------------------------------------------------------------------------------

// A value a tweak plugin sets (P1, docs/12 §3): checked against the allow-list (Tweaks.hpp).
enum class RegistryType
{
    Dword,
    String,
};

struct Tweak
{
    std::string key;   // under HKEY_CURRENT_USER, normalized: "Software\Microsoft\...\Advanced"
    std::string value; // value name ("" = the key's default value)
    RegistryType type = RegistryType::Dword;
    std::variant<std::uint64_t, std::string> data;
    // Explorer reads it only when it starts: the user is told to restart it (we never do).
    bool needsExplorerRestart = false;

    friend bool operator==(const Tweak&, const Tweak&) = default;
};

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
    std::optional<StartConfig> start;
    std::vector<BarConfig> bars;
    std::vector<PluginRequirement> pluginRequirements;
    std::vector<PluginSettings> pluginSettings;
    // Not in rice.toml: the values of the tweak plugins this rice requires, added by the core
    // (the "tweaks" module applies them).
    std::vector<Tweak> tweaks;

    const PaletteEntry* findPalette(std::string_view name) const noexcept;
};

// Enum <-> rice.toml spelling, shared by the parser, the exporter and error messages.
std::string_view toString(ThemeMode value) noexcept;
std::string_view toString(WallpaperLayout value) noexcept;
std::string_view toString(WallpaperKind value) noexcept;
std::string_view toString(WallpaperFit value) noexcept;
std::string_view toString(ParticleKind value) noexcept;
std::string_view toString(TaskbarAlignment value) noexcept;
std::string_view toString(TaskbarSearch value) noexcept;
std::string_view toString(BarPosition value) noexcept;
std::string_view toString(BarMode value) noexcept;
std::string_view toString(BarStyle value) noexcept;
std::string_view toString(StartMenuKind value) noexcept;
std::string_view toString(StartLayout value) noexcept;
std::string_view toString(BarBackground::Kind value) noexcept;

} // namespace rmw::domain
