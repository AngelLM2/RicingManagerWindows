#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/domain/Settings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What rmw-settings does with files and the rice model, without any UI (docs/04 M7): the rice
// library, reading and saving a rice and settings.toml, bringing the user's files into a rice,
// the bar editor's moves. Unit-tested; the app only shows it.
namespace rmw::settings
{

// ---- the library --------------------------------------------------------------------------------

// The rices the settings app keeps: <data>\rices\<folder>.
[[nodiscard]] std::filesystem::path ricesFolder(const std::filesystem::path& dataDirectory);

struct RiceEntry
{
    std::filesystem::path folder;
    std::string name; // from [meta] (the folder's name when unreadable)
    std::string author;
    std::string description;
    std::string problem;           // why it does not read (empty: it does)
    std::filesystem::path preview; // an image of it (its first wallpaper), absolute; may be empty
    bool shareable = true;         // false: it holds others' files (Wallpaper Engine), kept here
};

// What one folder holds. Never fails: an unreadable rice says why in `problem`.
[[nodiscard]] RiceEntry readEntry(const std::filesystem::path& folder);
// The library's rices plus `extra` folders (the active rice, folders the user added), each once,
// by name.
[[nodiscard]] std::vector<RiceEntry> scanLibrary(const std::filesystem::path& dataDirectory,
                                                 std::span<const std::filesystem::path> extra);
// A new, empty folder in the library for a rice called `name` ("My look", "My look 2"...).
[[nodiscard]] Result<std::filesystem::path> newRiceFolder(const std::filesystem::path& dataDirectory,
                                                          std::string_view name);
// A copy of a rice folder's files (no links followed), into a new folder of the library; its
// [meta] name gets `name`.
[[nodiscard]] Result<std::filesystem::path> duplicateRice(const std::filesystem::path& dataDirectory,
                                                          const std::filesystem::path& from,
                                                          std::string_view name);

// ---- a rice -----------------------------------------------------------------------------------

struct LoadedRice
{
    domain::Rice rice;
    domain::Diagnostics diagnostics; // warnings (errors make loadRice fail)
    bool handWritten = false;        // has comments the writer would drop
};

[[nodiscard]] Result<LoadedRice> loadRice(const std::filesystem::path& folder);
// Writes rice.toml (atomically) with a first line saying who wrote it.
[[nodiscard]] Result<void> saveRice(const std::filesystem::path& folder, const domain::Rice& rice);
inline constexpr std::string_view kWrittenByLine = "# Written by Ricing Manager (comments are not kept).";

// What a picked file or folder is for, and so where it goes in the rice and what it may be.
enum class AssetKind : std::uint8_t
{
    Image,     // assets/wallpapers: .jpg .jpeg .png .bmp
    Video,     // assets/videos: .mp4 .m4v
    Slideshow, // assets/slides/<name>: a folder of images
    Cursors,   // assets/cursors/<name>: a folder of .cur/.ani (+ install.inf)
};

// Copies the user's file (or folder) into the rice and returns its package path
// ("assets/wallpapers/city.jpg"). A file already there with the same name and content is reused;
// another name is found when the content differs. Links and reparse points are refused.
[[nodiscard]] Result<std::string> importAsset(const std::filesystem::path& riceFolder,
                                              const std::filesystem::path& source, AssetKind kind);

// The absolute path of a package path of the rice ("" when the path is not one).
[[nodiscard]] std::filesystem::path assetPath(const std::filesystem::path& riceFolder,
                                              std::string_view packagePath);

// ---- the wallpaper ----------------------------------------------------------------------------

// The rule for `match` in the rice's per-monitor wallpaper (made, as an image rule, when missing).
domain::MonitorWallpaper& wallpaperRule(domain::Rice& rice, const domain::MonitorSelector& match);
void removeWallpaperRule(domain::Rice& rice, const domain::MonitorSelector& match);

// ---- the bar ----------------------------------------------------------------------------------

enum class Zone : std::uint8_t
{
    Left,
    Center,
    Right,
};

// The widgets a bar can show by name (docs/04 M3), in the order the add menu lists them.
[[nodiscard]] std::span<const std::string_view> builtinWidgets() noexcept;

std::vector<domain::BarItem>& zoneOf(domain::BarConfig& bar, Zone zone);
// Moves an item; `to` is clamped to the target zone. Marks both zones as written.
void moveItem(domain::BarConfig& bar, Zone from, std::size_t index, Zone to, std::size_t toIndex);
void addItem(domain::BarConfig& bar, Zone zone, std::string id);
void removeItem(domain::BarConfig& bar, Zone zone, std::size_t index);
// The bar now writes `key` itself (rice.toml), instead of leaving it to its skin.
void markKey(domain::BarConfig& bar, std::string_view key);
// A new bar, set to show a usual taskbar.
[[nodiscard]] domain::BarConfig newBar();

// ---- settings.toml ----------------------------------------------------------------------------

[[nodiscard]] std::filesystem::path settingsFile(const std::filesystem::path& dataDirectory);
// No file: empty settings. A broken one fails (the app shows why and does not overwrite it).
[[nodiscard]] Result<domain::Settings> loadSettings(const std::filesystem::path& dataDirectory,
                                                    std::span<const std::string_view> knownActions);
[[nodiscard]] Result<void> saveSettings(const std::filesystem::path& dataDirectory,
                                        const domain::Settings& settings);

} // namespace rmw::settings
