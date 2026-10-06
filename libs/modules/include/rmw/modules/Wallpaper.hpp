#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Display.hpp"
#include "rmw/ports/Registry.hpp"

#include <optional>
#include <span>
#include <string>

// Static wallpaper per monitor (docs/03 §6, ADR-0004). Fields:
//   "monitor:<identityKey>"  {"file": path} — "" means no image; captures may add "ext" (type of
//                            an extension-less file to copy) and "keep" (a Windows file)
//   "position"               DESKTOP_WALLPAPER_POSITION (global in Windows)
//   "background"             "#RRGGBB" (global)
//   "slideshow"              null, or {"items": [paths], "shuffle": bool, "intervalMs": n}
//   "backgroundType"         Explorer\Wallpapers\BackgroundType (0 picture, 1 color,
//                            2 slideshow, 3 Spotlight) or null
// IDesktopWallpaper has one position and one background color for all monitors. Until
// pre-rendering arrives, the first rule's fit is used everywhere (the parser warns), and a
// per-monitor "color" becomes a small solid image with the monitor's aspect ratio.
namespace rmw::modules
{

class WallpaperLogic final : public engine::IModuleLogic
{
public:
    explicit WallpaperLogic(ports::IDisplayTopology& topology) noexcept
        : m_topology(topology)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "wallpaper"; }
    [[nodiscard]] std::span<const std::string_view> dependsOn() const noexcept override;
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    // Copies the images shown now into the store (Windows' own files excepted); when the
    // original file is gone, Windows' TranscodedWallpaper copy is used (ADR-0004 decision 3).
    [[nodiscard]] Result<void> internalize(engine::Json& state, engine::IAssetSink& assets) const override;
    [[nodiscard]] bool fieldEquivalent(std::string_view field, const engine::Json& a,
                                       const engine::Json& b) const override;

private:
    [[nodiscard]] Result<engine::Json> desiredFields(const domain::Rice& rice, engine::IAssetSink& assets) const;

    ports::IDisplayTopology& m_topology;
};

class WallpaperSystem final : public engine::IModuleSystem
{
public:
    WallpaperSystem(ports::IDisplayTopology& topology, ports::IWallpaperApi& wallpaper,
                    ports::IRegistry& registry) noexcept
        : m_topology(topology)
        , m_wallpaper(wallpaper)
        , m_registry(registry)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "wallpaper"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    struct Slot
    {
        ports::MonitorInfo monitor;
        std::string wallpaperPath; // IDesktopWallpaper's name for it
    };
    // Pairs each connected monitor with IDesktopWallpaper's path by its rectangle (the device
    // path from QueryDisplayConfig can be empty).
    [[nodiscard]] Result<std::vector<Slot>> slots();
    // The picture a monitor field asks for, as far as it is there: the file, else its `copy`, else
    // Windows' own wallpaper; nothing when none is ("" is no picture at all).
    [[nodiscard]] std::optional<std::string> resolve(const engine::Json& monitor);

    ports::IDisplayTopology& m_topology;
    ports::IWallpaperApi& m_wallpaper;
    ports::IRegistry& m_registry;
};

inline constexpr std::string_view kMonitorFieldPrefix = "monitor:";

// A slideshow Windows cannot play (another folder, or something else, on other monitors): the
// core's scheduler turns its pages (docs/04 M2). {"monitor:<key>": {"items": [stored paths],
// "intervalMs": n, "shuffle": bool}}; empty when Windows' own slideshow (or none) does the job.
[[nodiscard]] Result<engine::Json> wallpaperRotation(const domain::WallpaperConfig& config,
                                                     std::span<const ports::MonitorInfo> monitors,
                                                     engine::IAssetSink& assets);

// The page such a slideshow shows next: the following one, or with `shuffle` any other one
// (`random` picks it). `current` == count when what is shown now is not one of the pages.
[[nodiscard]] std::size_t nextPage(std::size_t count, std::size_t current, bool shuffle,
                                   std::uint64_t random) noexcept;

} // namespace rmw::modules
