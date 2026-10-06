#include "rmw/settings/WallpaperEngine.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <set>

namespace rmw::settings
{
namespace
{

namespace fs = std::filesystem;

constexpr std::wstring_view kWallpaperEngineApp = L"431960"; // its Steam app id
constexpr std::uint64_t kProjectMaxBytes = 1u << 20;
constexpr std::uint64_t kVdfMaxBytes = 1u << 20;
constexpr std::array<std::wstring_view, 3> kVideoExtensions = {L".mp4", L".m4v", L".webm"};
constexpr std::array<std::wstring_view, 5> kPictureExtensions = {L".jpg", L".jpeg", L".png", L".bmp", L".gif"};

std::wstring lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

template <std::size_t N>
bool hasExtension(const fs::path& path, const std::array<std::wstring_view, N>& extensions)
{
    const std::wstring extension = lower(path.extension().native());
    return std::ranges::find(extensions, extension) != extensions.end();
}

// `name` from project.json as a file of `folder`: only a plain name inside it, and only a file.
fs::path childFile(const fs::path& folder, std::string_view name)
{
    if (name.empty() || name.size() > 260)
    {
        return {};
    }
    const fs::path relative(utf8To16Lossy(name));
    if (relative.has_root_name() || relative.has_root_directory())
    {
        return {};
    }
    for (const auto& part : relative)
    {
        if (part == L".." || part == L".")
        {
            return {};
        }
    }
    const fs::path file = folder / relative;
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(file, ec)) ? file : fs::path();
}

EngineKind kindOf(std::string type)
{
    std::ranges::transform(type, type.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (type == "video")
    {
        return EngineKind::Video;
    }
    if (type == "scene")
    {
        return EngineKind::Scene;
    }
    if (type == "web")
    {
        return EngineKind::Web;
    }
    if (type == "application")
    {
        return EngineKind::Application;
    }
    return EngineKind::Other;
}

std::optional<fs::path> registryPath(HKEY root, const wchar_t* key, const wchar_t* value)
{
    std::array<wchar_t, MAX_PATH * 2> buffer{};
    auto size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return fs::path(buffer.data()).make_preferred();
}

// Where Wallpaper Engine's wallpapers may be in one Steam library.
std::vector<fs::path> wallpaperFolders(const fs::path& library)
{
    const fs::path apps = library / L"steamapps";
    const fs::path engine = apps / L"common" / L"wallpaper_engine" / L"projects";
    return {apps / L"workshop" / L"content" / fs::path(kWallpaperEngineApp), engine / L"myprojects",
            engine / L"defaultprojects"};
}

} // namespace

std::string_view toString(EngineKind kind) noexcept
{
    switch (kind)
    {
    case EngineKind::Video:
        return "video";
    case EngineKind::Scene:
        return "scene";
    case EngineKind::Web:
        return "web";
    case EngineKind::Application:
        return "application";
    case EngineKind::Other:
        return "other";
    }
    return "other";
}

std::vector<fs::path> steamLibraries(std::string_view vdf)
{
    // Quoted tokens; a "path" key is followed by its value. Inside quotes, \\ is a backslash.
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < vdf.size(); ++i)
    {
        if (vdf[i] != '"')
        {
            continue;
        }
        std::string token;
        for (++i; i < vdf.size() && vdf[i] != '"'; ++i)
        {
            if (vdf[i] == '\\' && i + 1 < vdf.size())
            {
                token += vdf[++i];
            }
            else
            {
                token += vdf[i];
            }
        }
        tokens.push_back(std::move(token));
    }
    std::vector<fs::path> libraries;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        std::string key = tokens[i];
        std::ranges::transform(key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (key == "path" && !tokens[i + 1].empty())
        {
            fs::path library(utf8To16Lossy(tokens[i + 1]));
            library.make_preferred();
            if (library.is_absolute() && std::ranges::find(libraries, library) == libraries.end())
            {
                libraries.push_back(std::move(library));
            }
            ++i;
        }
    }
    return libraries;
}

Result<EngineWallpaper> readEngineProject(const fs::path& folder)
{
    auto bytes = platform::readFileLimited(folder / L"project.json", kProjectMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3); // a BOM, as some editors write
    }
    auto parsed = json::parse(text);
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()).withContext("project.json"));
    }
    if (!parsed->is_object())
    {
        return fail(ErrorCode::InvalidData, "project.json: not an object");
    }
    const json::Json& project = *parsed;
    EngineWallpaper wallpaper;
    wallpaper.folder = folder;
    wallpaper.title = json::stringOr(project, "title", "");
    if (wallpaper.title.empty())
    {
        wallpaper.title = utf16To8Lossy(folder.filename().native());
    }
    const auto id = project.find("workshopid");
    if (id != project.end() && id->is_string())
    {
        wallpaper.id = id->get<std::string>();
    }
    else if (id != project.end() && id->is_number_unsigned())
    {
        wallpaper.id = std::to_string(id->get<std::uint64_t>());
    }
    else
    {
        wallpaper.id = utf16To8Lossy(folder.filename().native());
    }
    wallpaper.kind = kindOf(json::stringOr(project, "type", ""));
    if (wallpaper.kind == EngineKind::Video)
    {
        const fs::path file = childFile(folder, json::stringOr(project, "file", ""));
        if (!file.empty() && hasExtension(file, kVideoExtensions))
        {
            wallpaper.video = file;
        }
    }
    const fs::path preview = childFile(folder, json::stringOr(project, "preview", ""));
    if (!preview.empty() && hasExtension(preview, kPictureExtensions))
    {
        wallpaper.preview = preview;
    }
    return wallpaper;
}

std::vector<EngineWallpaper> findEngineWallpapers(std::span<const fs::path> extra, std::size_t limit)
{
    std::vector<fs::path> libraries;
    std::optional<fs::path> steam = registryPath(HKEY_CURRENT_USER, LR"(Software\Valve\Steam)", L"SteamPath");
    if (!steam)
    {
        steam = registryPath(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\WOW6432Node\Valve\Steam)", L"InstallPath");
    }
    if (steam)
    {
        libraries.push_back(*steam);
        if (auto vdf = platform::readFileLimited(*steam / L"steamapps" / L"libraryfolders.vdf", kVdfMaxBytes))
        {
            for (auto& library : steamLibraries(std::string_view(reinterpret_cast<const char*>(vdf->data()), vdf->size())))
            {
                if (std::ranges::find(libraries, library) == libraries.end())
                {
                    libraries.push_back(std::move(library));
                }
            }
        }
    }

    // Folders that may hold wallpapers (each a folder of them), and wallpapers pointed at directly.
    std::vector<fs::path> parents;
    std::vector<fs::path> projects;
    for (const auto& library : libraries)
    {
        for (auto& folder : wallpaperFolders(library))
        {
            parents.push_back(std::move(folder));
        }
    }
    std::error_code ec;
    for (const auto& folder : extra)
    {
        (fs::exists(folder / L"project.json", ec) ? projects : parents).push_back(folder);
    }
    for (const auto& parent : parents)
    {
        for (auto it = fs::directory_iterator(parent, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            if (it->is_directory(ec) && !it->is_symlink(ec) && fs::exists(it->path() / L"project.json", ec))
            {
                projects.push_back(it->path());
            }
            if (projects.size() >= limit)
            {
                break;
            }
        }
        ec.clear();
    }

    std::vector<EngineWallpaper> found;
    std::set<std::wstring> seen;
    for (const auto& folder : projects)
    {
        if (found.size() >= limit)
        {
            break;
        }
        const std::wstring key = lower(fs::weakly_canonical(folder, ec).native());
        if (!seen.insert(key).second)
        {
            continue;
        }
        if (auto wallpaper = readEngineProject(folder))
        {
            found.push_back(std::move(*wallpaper));
        }
    }
    std::ranges::sort(found, [](const EngineWallpaper& a, const EngineWallpaper& b) {
        return std::ranges::lexicographical_compare(a.title, b.title, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    });
    return found;
}

} // namespace rmw::settings
