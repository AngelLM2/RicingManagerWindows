#include "rmw/settings/Model.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <fstream>
#include <initializer_list>

namespace rmw::settings
{
namespace
{

namespace fs = std::filesystem;

constexpr std::uint64_t kVideoMaxBytes = std::uint64_t{4} << 30; // the core's own limit
constexpr std::uint64_t kSettingsMaxBytes = 1u << 20;
constexpr std::size_t kFolderFilesMax = 500;

constexpr std::initializer_list<std::string_view> kImageExtensions = {"jpg", "jpeg", "png", "bmp"};
constexpr std::initializer_list<std::string_view> kVideoExtensions = {"mp4", "m4v", "webm"};
constexpr std::initializer_list<std::string_view> kCursorExtensions = {"cur", "ani", "inf"};

constexpr auto kBuiltinWidgets = std::to_array<std::string_view>({
    "start",
    "desktops",
    "windows",
    "tray",
    "clock",
    "media",
    "cpu",
    "memory",
    "network",
    "volume",
    "battery",
    "notifications",
});

std::string utf8(const fs::path& path)
{
    return utf16To8Lossy(path.native());
}

bool isLink(const fs::path& path)
{
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    return ec || fs::is_symlink(status) || fs::is_other(status);
}

// A name that is a valid package path component: letters, digits, spaces, '.', '-', '_' (others
// become '-'), no trailing dots or spaces, not too long.
Result<platform::Sha256Digest> hashFile(const fs::path& file)
{
    auto hash = platform::Sha256::create();
    if (!hash)
    {
        return std::unexpected(std::move(hash.error()));
    }
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return fail(ErrorCode::Io, std::format("cannot read {}", utf8(file)));
    }
    std::array<char, std::size_t{64} * 1024> buffer{};
    while (in)
    {
        in.read(buffer.data(), buffer.size());
        const auto read = static_cast<std::size_t>(in.gcount());
        if (read == 0)
        {
            break;
        }
        if (auto updated = hash->update(std::as_bytes(std::span(buffer.data(), read))); !updated)
        {
            return std::unexpected(std::move(updated.error()));
        }
    }
    return hash->finish();
}

bool sameContent(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec)
    {
        return false;
    }
    auto first = hashFile(a);
    auto second = hashFile(b);
    return first && second && *first == *second;
}

// Copies one file into `folder` under `name` (or "name 2.ext"...): the same content already there
// is reused. Returns the file name used.
Result<std::string> copyInto(const fs::path& source, const fs::path& folder, const std::string& name,
                             std::uint64_t maxBytes)
{
    if (isLink(source))
    {
        return fail(ErrorCode::AccessDenied, std::format("{} is a link; pick the file itself", utf8(source)));
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path base(utf8To16Lossy(name));
    for (int attempt = 1; attempt < 1000; ++attempt)
    {
        const std::wstring stem = base.stem().native();
        const fs::path candidate =
            attempt == 1 ? base : fs::path(std::format(L"{} {}{}", stem, attempt, base.extension().native()));
        const fs::path target = folder / candidate;
        if (fs::exists(target, ec))
        {
            if (sameContent(source, target))
            {
                return utf8(candidate);
            }
            continue;
        }
        if (auto copied = platform::copyHashed(source, target, maxBytes); !copied)
        {
            return std::unexpected(std::move(copied.error()));
        }
        return utf8(candidate);
    }
    return fail(ErrorCode::AlreadyExists, std::format("too many files called {}", name));
}

std::string extensionOf(const fs::path& path)
{
    std::string extension = utf8(path.extension());
    if (!extension.empty())
    {
        extension.erase(0, 1);
    }
    std::ranges::transform(extension, extension.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return extension;
}

bool hasOneOf(const fs::path& path, std::initializer_list<std::string_view> extensions)
{
    return std::ranges::find(extensions, extensionOf(path)) != extensions.end();
}

std::string zoneKey(Zone zone)
{
    switch (zone)
    {
    case Zone::Left:
        return "left";
    case Zone::Center:
        return "center";
    case Zone::Right:
        return "right";
    }
    return "left";
}

bool equivalentFolders(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    const auto left = fs::absolute(a, ec).lexically_normal().native();
    const auto right = fs::absolute(b, ec).lexically_normal().native();
    return std::ranges::equal(left, right,
                              [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

} // namespace

// ---- the library --------------------------------------------------------------------------------

fs::path ricesFolder(const fs::path& dataDirectory)
{
    return dataDirectory / L"rices";
}

RiceEntry readEntry(const fs::path& folder)
{
    RiceEntry entry;
    entry.folder = folder;
    entry.name = utf8(folder.filename());
    auto loaded = loadRice(folder);
    if (!loaded)
    {
        entry.problem = loaded.error().message();
        return entry;
    }
    const domain::Rice& rice = loaded->rice;
    entry.name = rice.meta.name;
    entry.author = rice.meta.author;
    entry.description = rice.meta.description;
    entry.shareable = rice.meta.canShare();
    if (rice.wallpaper)
    {
        if (rice.wallpaper->layout == domain::WallpaperLayout::Span)
        {
            entry.preview = assetPath(folder, rice.wallpaper->spanSource);
        }
        for (const auto& rule : rice.wallpaper->monitors)
        {
            if (!entry.preview.empty())
            {
                break;
            }
            if (rule.kind == domain::WallpaperKind::Image)
            {
                entry.preview = assetPath(folder, rule.source);
            }
            else if (rule.kind == domain::WallpaperKind::Video && !rule.fallbackImage.empty())
            {
                entry.preview = assetPath(folder, rule.fallbackImage);
            }
        }
    }
    return entry;
}

std::vector<RiceEntry> scanLibrary(const fs::path& dataDirectory, std::span<const fs::path> extra)
{
    std::vector<fs::path> folders;
    std::error_code ec;
    for (fs::directory_iterator it(ricesFolder(dataDirectory), ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_directory(ec) && !isLink(it->path()) && fs::exists(it->path() / L"rice.toml", ec))
        {
            folders.push_back(it->path());
        }
    }
    for (const auto& folder : extra)
    {
        if (!folder.empty() &&
            std::ranges::none_of(folders, [&](const fs::path& f) { return equivalentFolders(f, folder); }))
        {
            folders.push_back(folder);
        }
    }
    std::vector<RiceEntry> entries;
    entries.reserve(folders.size());
    for (const auto& folder : folders)
    {
        entries.push_back(readEntry(folder));
    }
    std::ranges::sort(entries, [](const RiceEntry& a, const RiceEntry& b) {
        return std::ranges::lexicographical_compare(a.name, b.name, [](char x, char y) {
            const auto lower = [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(x) < lower(y);
        });
    });
    return entries;
}

Result<fs::path> newRiceFolder(const fs::path& dataDirectory, std::string_view name)
{
    const std::string base = domain::safeFileName(name, "Rice");
    const fs::path root = ricesFolder(dataDirectory);
    std::error_code ec;
    fs::create_directories(root, ec);
    for (int attempt = 1; attempt < 1000; ++attempt)
    {
        const std::string folderName = attempt == 1 ? base : std::format("{} {}", base, attempt);
        fs::path folder = root / fs::path(utf8To16Lossy(folderName));
        if (fs::exists(folder, ec))
        {
            continue;
        }
        if (!fs::create_directory(folder, ec) || ec)
        {
            return fail(ErrorCode::Io, std::format("cannot create {}: {}", utf8(folder), ec.message()));
        }
        return folder;
    }
    return fail(ErrorCode::AlreadyExists, std::format("too many rices called {}", base));
}

Result<fs::path> duplicateRice(const fs::path& dataDirectory, const fs::path& from, std::string_view name)
{
    auto loaded = loadRice(from);
    if (!loaded)
    {
        return std::unexpected(std::move(loaded.error()));
    }
    auto folder = newRiceFolder(dataDirectory, name);
    if (!folder)
    {
        return folder;
    }
    std::error_code ec;
    std::size_t copied = 0;
    for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec))
    {
        if (isLink(it->path()))
        {
            if (it->is_directory(ec))
            {
                it.disable_recursion_pending();
            }
            continue; // links are never followed nor copied
        }
        const fs::path relative = it->path().lexically_relative(from);
        if (it->is_directory(ec))
        {
            fs::create_directories(*folder / relative, ec);
            continue;
        }
        if (relative == L"rice.toml" || ++copied > limits::kPackageMaxEntries)
        {
            continue;
        }
        if (auto copy = platform::copyHashed(it->path(), *folder / relative, kVideoMaxBytes); !copy)
        {
            return std::unexpected(std::move(copy.error()));
        }
    }
    loaded->rice.meta.name = std::string(name);
    loaded->rice.meta.id.clear(); // a copy is a rice of its own
    if (auto saved = saveRice(*folder, loaded->rice); !saved)
    {
        return std::unexpected(std::move(saved.error()));
    }
    return folder;
}

// ---- a rice -----------------------------------------------------------------------------------

Result<LoadedRice> loadRice(const fs::path& folder)
{
    auto bytes = platform::readFileLimited(folder / L"rice.toml", limits::kManifestMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    LoadedRice loaded;
    auto rice = domain::parseRice(text, loaded.diagnostics);
    if (!rice)
    {
        return std::unexpected(std::move(rice.error()));
    }
    loaded.rice = std::move(*rice);
    // Comments of the author's own, which saving would drop. Ours are not: the line that saving
    // writes, and the note `rmw export` opens a file with.
    std::size_t start = 0;
    if (text.starts_with("# Exported by Ricing Manager"))
    {
        while (start < text.size() && text[start] == '#')
        {
            start = std::min(text.find('\n', start), text.size()) + 1;
        }
    }
    while (start < text.size())
    {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        {
            line.remove_prefix(1);
        }
        if (line.starts_with('#') && !line.starts_with(kWrittenByLine))
        {
            loaded.handWritten = true;
            break;
        }
        start = end + 1;
    }
    return loaded;
}

Result<void> saveRice(const fs::path& folder, const domain::Rice& rice)
{
    const std::string text = std::string(kWrittenByLine) + "\n" + domain::writeRice(rice);
    return platform::writeFileAtomic(folder / L"rice.toml", std::as_bytes(std::span(text)));
}

Result<std::string> importAsset(const fs::path& riceFolder, const fs::path& source, AssetKind kind)
{
    std::error_code ec;
    const bool folderKind = kind == AssetKind::Slideshow || kind == AssetKind::Cursors;
    if (folderKind != fs::is_directory(source, ec))
    {
        return fail(ErrorCode::InvalidArgument,
                    folderKind ? "pick a folder" : std::format("{} is not a file", utf8(source)));
    }
    if (kind == AssetKind::Image || kind == AssetKind::Video)
    {
        const bool image = kind == AssetKind::Image;
        if (!hasOneOf(source, image ? kImageExtensions : kVideoExtensions))
        {
            return fail(ErrorCode::Unsupported,
                        image ? "pick a .jpg, .png or .bmp image" : "pick an .mp4 or .webm video");
        }
        if (image && platform::sniffFileType(source).empty())
        {
            return fail(ErrorCode::InvalidData, std::format("{} is not an image", utf8(source)));
        }
        const std::string sub = image ? "assets/wallpapers" : "assets/videos";
        const std::string name =
            domain::safeFileName(utf8(source.filename()), image ? "wallpaper.jpg" : "video.mp4");
        auto stored = copyInto(source, riceFolder / fs::path(utf8To16Lossy(sub)), name,
                               image ? limits::kImageMaxBytes : kVideoMaxBytes);
        if (!stored)
        {
            return stored;
        }
        return sub + "/" + *stored;
    }
    // A folder: its images (or cursor files), not its subfolders.
    const bool slides = kind == AssetKind::Slideshow;
    const std::string sub = slides ? "assets/slides/" : "assets/cursors/";
    std::string folderName = domain::safeFileName(utf8(source.filename()), slides ? "slides" : "cursors");
    fs::path target = riceFolder / fs::path(utf8To16Lossy(sub + folderName));
    for (int attempt = 2; fs::exists(target, ec) && attempt < 1000; ++attempt)
    {
        folderName = domain::safeFileName(std::format("{} {}", utf8(source.filename()), attempt), "folder");
        target = riceFolder / fs::path(utf8To16Lossy(sub + folderName));
    }
    std::size_t count = 0;
    for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || isLink(it->path()) ||
            !hasOneOf(it->path(), slides ? kImageExtensions : kCursorExtensions))
        {
            continue;
        }
        if (++count > kFolderFilesMax)
        {
            break;
        }
        const std::string name = domain::safeFileName(utf8(it->path().filename()), "file");
        if (auto stored =
                copyInto(it->path(), target, name, slides ? limits::kImageMaxBytes : limits::kCursorMaxBytes);
            !stored)
        {
            return stored;
        }
    }
    if (count == 0)
    {
        return fail(ErrorCode::NotFound, slides ? "the folder has no .jpg, .png or .bmp images"
                                                : "the folder has no .cur or .ani cursors");
    }
    return sub + folderName;
}

fs::path assetPath(const fs::path& riceFolder, std::string_view packagePath)
{
    auto checked = domain::validatePackagePath(packagePath);
    if (!checked)
    {
        return {};
    }
    return (riceFolder / fs::path(utf8To16Lossy(*checked))).make_preferred();
}

// ---- the wallpaper ----------------------------------------------------------------------------

domain::MonitorWallpaper& wallpaperRule(domain::Rice& rice, const domain::MonitorSelector& match)
{
    if (!rice.wallpaper)
    {
        rice.wallpaper = domain::WallpaperConfig{};
    }
    auto& rules = rice.wallpaper->monitors;
    if (const auto found = std::ranges::find(rules, match, &domain::MonitorWallpaper::match);
        found != rules.end())
    {
        return *found;
    }
    domain::MonitorWallpaper rule;
    rule.match = match;
    // "all" takes what the others leave: it stays last.
    const auto all = std::ranges::find_if(
        rules, [](const auto& r) { return r.match.kind == domain::MonitorSelector::Kind::All; });
    if (match.kind != domain::MonitorSelector::Kind::All && all != rules.end())
    {
        return *rules.insert(all, std::move(rule));
    }
    rules.push_back(std::move(rule));
    return rules.back();
}

void removeWallpaperRule(domain::Rice& rice, const domain::MonitorSelector& match)
{
    if (rice.wallpaper)
    {
        std::erase_if(rice.wallpaper->monitors, [&](const auto& rule) { return rule.match == match; });
    }
}

// ---- the bar ----------------------------------------------------------------------------------

std::span<const std::string_view> builtinWidgets() noexcept
{
    return kBuiltinWidgets;
}

std::vector<domain::BarItem>& zoneOf(domain::BarConfig& bar, Zone zone)
{
    switch (zone)
    {
    case Zone::Left:
        return bar.left;
    case Zone::Center:
        return bar.center;
    case Zone::Right:
        return bar.right;
    }
    return bar.left;
}

void markKey(domain::BarConfig& bar, std::string_view key)
{
    // A bar that names no keys and has no skin writes them all already.
    if ((bar.keys.empty() && bar.skin.empty()) || std::ranges::contains(bar.keys, key))
    {
        return;
    }
    bar.keys.emplace_back(key);
}

void moveItem(domain::BarConfig& bar, Zone from, std::size_t index, Zone to, std::size_t toIndex)
{
    auto& source = zoneOf(bar, from);
    if (index >= source.size())
    {
        return;
    }
    domain::BarItem item = std::move(source[index]);
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(index));
    auto& target = zoneOf(bar, to);
    const std::size_t at = std::min(toIndex, target.size());
    target.insert(target.begin() + static_cast<std::ptrdiff_t>(at), std::move(item));
    markKey(bar, zoneKey(from));
    markKey(bar, zoneKey(to));
}

void addItem(domain::BarConfig& bar, Zone zone, std::string id)
{
    auto& items = zoneOf(bar, zone);
    if (items.size() < limits::kBarMaxItemsPerZone)
    {
        items.push_back({std::move(id), {}});
        markKey(bar, zoneKey(zone));
    }
}

void removeItem(domain::BarConfig& bar, Zone zone, std::size_t index)
{
    auto& items = zoneOf(bar, zone);
    if (index < items.size())
    {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
        markKey(bar, zoneKey(zone));
    }
}

domain::BarConfig newBar()
{
    domain::BarConfig bar;
    bar.monitor = *domain::MonitorSelector::parse("primary");
    bar.position = domain::BarPosition::Bottom;
    bar.mode = domain::BarMode::Reserve;
    bar.thickness = 44;
    bar.background.kind = domain::BarBackground::Kind::Acrylic;
    bar.background.opacity = 0.6;
    bar.hideNativeTaskbar = true;
    for (const auto* id : {"start", "desktops", "windows"})
    {
        bar.left.push_back({id, {}});
    }
    bar.center.push_back({"clock", {}});
    for (const auto* id : {"tray", "volume", "network", "battery", "notifications"})
    {
        bar.right.push_back({id, {}});
    }
    return bar;
}

// ---- settings.toml ----------------------------------------------------------------------------

fs::path settingsFile(const fs::path& dataDirectory)
{
    return dataDirectory / L"config" / L"settings.toml";
}

Result<domain::Settings> loadSettings(const fs::path& dataDirectory,
                                      std::span<const std::string_view> knownActions)
{
    const fs::path file = settingsFile(dataDirectory);
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        return domain::Settings{};
    }
    auto bytes = platform::readFileLimited(file, kSettingsMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    domain::Diagnostics diagnostics;
    return domain::parseSettings(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), diagnostics,
        knownActions);
}

Result<void> saveSettings(const fs::path& dataDirectory, const domain::Settings& settings)
{
    const fs::path file = settingsFile(dataDirectory);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const std::string text = std::string(kWrittenByLine) + "\n" + domain::writeSettings(settings);
    return platform::writeFileAtomic(file, std::as_bytes(std::span(text)));
}

} // namespace rmw::settings
