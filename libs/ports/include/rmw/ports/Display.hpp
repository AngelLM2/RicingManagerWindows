#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::ports
{

// A connected, active monitor (docs/03 §6, ADR-0004).
struct MonitorInfo
{
    std::string devicePath;  // \\?\DISPLAY#... as IDesktopWallpaper names it; may be empty
    std::string identityKey; // stable across ports/docks when EDID allows (ADR-0004 decision 1)
    std::string displayName; // EDID name, friendly name, or "Monitor N (WxH)"
    bool primary = false;
    std::int32_t left = 0; // virtual-screen pixels
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    [[nodiscard]] std::int32_t width() const noexcept { return right - left; }
    [[nodiscard]] std::int32_t height() const noexcept { return bottom - top; }
};

// Read-only display topology (QueryDisplayConfig + EDID). Never talks to Explorer, so the core
// may call it directly.
class IDisplayTopology
{
public:
    virtual ~IDisplayTopology() = default;
    [[nodiscard]] virtual Result<std::vector<MonitorInfo>> monitors() = 0;
};

// DESKTOP_WALLPAPER_POSITION values.
enum class WallpaperPosition : std::uint32_t
{
    Center = 0,
    Tile = 1,
    Stretch = 2,
    Fit = 3,
    Fill = 4,
    Span = 5,
};

struct SlideshowState
{
    bool active = false;
    std::vector<std::string> items; // image files (UTF-8)
    bool shuffle = false;
    std::uint32_t intervalMs = 0;
};

// A monitor as IDesktopWallpaper names it. Its rectangle is how it is matched to MonitorInfo:
// QueryDisplayConfig does not always report the device path (seen on real hardware).
struct WallpaperMonitor
{
    std::string devicePath;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// IDesktopWallpaper (talks to Explorer: runs in the apply helper). Paths are UTF-8.
class IWallpaperApi
{
public:
    virtual ~IWallpaperApi() = default;

    // Attached monitors only (paths of disconnected ones have no rectangle).
    [[nodiscard]] virtual Result<std::vector<WallpaperMonitor>> monitors() = 0;

    // "" when the monitor shows no image (solid background).
    [[nodiscard]] virtual Result<std::string> wallpaper(std::string_view devicePath) = 0;
    // "" removes the image.
    [[nodiscard]] virtual Result<void> setWallpaper(std::string_view devicePath, std::string_view file) = 0;
    // One picture on every monitor, the way Settings sets it (SPI_SETDESKWALLPAPER): Windows then names
    // that file as the wallpaper (Control Panel\Desktop\WallPaper), not its own copy.
    [[nodiscard]] virtual Result<void> setDesktopWallpaper(std::string_view file) = 0;

    [[nodiscard]] virtual Result<WallpaperPosition> position() = 0;
    [[nodiscard]] virtual Result<void> setPosition(WallpaperPosition position) = 0;

    // 0xRRGGBB
    [[nodiscard]] virtual Result<std::uint32_t> backgroundColor() = 0;
    [[nodiscard]] virtual Result<void> setBackgroundColor(std::uint32_t rgb) = 0;

    [[nodiscard]] virtual Result<SlideshowState> slideshow() = 0;
    // Starts the native slideshow over these files.
    [[nodiscard]] virtual Result<void> setSlideshow(const SlideshowState& state) = 0;

    // Where Windows keeps its copy of the current image (%APPDATA%\...\TranscodedWallpaper).
    [[nodiscard]] virtual Result<std::string> transcodedWallpaper() = 0;
    // Whether a file exists, and its type ("jpg", "png", "bmp"...) by content; empty if unknown.
    [[nodiscard]] virtual Result<std::string> sniffImageType(std::string_view file) = 0;
    // True for files Windows ships (under the Windows folder): they never need a copy.
    [[nodiscard]] virtual bool isWindowsFile(std::string_view file) = 0;
};

} // namespace rmw::ports
