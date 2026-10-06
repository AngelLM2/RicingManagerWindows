#pragma once

#include "rmw/base/Error.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Wallpaper Engine wallpapers this computer already has (M12, doc 14): read where Steam keeps
// them, never downloaded, never changed. A video plays as it is; a scene, a web page or an
// application cannot play outside Wallpaper Engine, so its preview picture stands in for it.
namespace rmw::settings
{

enum class EngineKind : std::uint8_t
{
    Video,
    Scene,
    Web,
    Application,
    Other,
};

struct EngineWallpaper
{
    std::string id; // the workshop item's number, or the folder's name
    std::string title;
    EngineKind kind = EngineKind::Other;
    std::filesystem::path folder;
    std::filesystem::path video;   // Kind::Video: the file (absolute); else empty
    std::filesystem::path preview; // its picture (absolute); may be empty

    // A video we can play, or a picture to show.
    [[nodiscard]] bool usable() const noexcept { return !video.empty() || !preview.empty(); }
};

[[nodiscard]] std::string_view toString(EngineKind kind) noexcept;

// The library folders listed in Steam's libraryfolders.vdf ("path" entries, unescaped).
[[nodiscard]] std::vector<std::filesystem::path> steamLibraries(std::string_view vdf);

// The wallpaper whose project.json is in `folder`. Its files must be in that folder: a name that
// leads elsewhere ("..\", a drive) is left out, like a file that is not there.
[[nodiscard]] Result<EngineWallpaper> readEngineProject(const std::filesystem::path& folder);

// Every wallpaper found: the workshop items of every Steam library (Steam's own folder from the
// registry), Wallpaper Engine's own projects, and `extra` folders the user pointed at (a wallpaper,
// or a folder of them). Unreadable ones are skipped; at most `limit`. Sorted by title.
[[nodiscard]] std::vector<EngineWallpaper> findEngineWallpapers(std::span<const std::filesystem::path> extra,
                                                                std::size_t limit = 2000);

} // namespace rmw::settings
