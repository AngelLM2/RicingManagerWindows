#include "rmw/modules/Export.hpp"

#include "rmw/base/Version.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/modules/Wallpaper.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace rmw::modules
{
namespace
{

using json::Json;

constexpr std::array<std::string_view, 6> kFitNames = {"center", "tile", "stretch", "fit", "fill", "span"};
constexpr std::array<std::string_view, 4> kSearchNames = {"hidden", "icon", "box", "button"};

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

void exportAppearance(const Json& state, std::string& toml, std::vector<std::string>& notes)
{
    if (!state.is_object())
    {
        return;
    }
    toml += "\n[appearance]\n";
    const bool apps = json::boolOr(state, "appsLight", true);
    const bool system = json::boolOr(state, "systemLight", true);
    if (apps != system)
    {
        notes.emplace_back("apps and Windows use different light/dark modes; exported Windows' mode");
    }
    toml += std::format("mode = {}\n", tomlString(system ? "light" : "dark"));
    if (const std::string accent = json::stringOr(state, "accent", ""); domain::Color::parse(accent))
    {
        toml += std::format("accent = {}\n", tomlString(accent));
    }
    toml += std::format("transparency = {}\n", json::boolOr(state, "transparency", true));
    toml += std::format("accent_on_taskbar = {}\n", json::boolOr(state, "accentOnTaskbar", false));
}

void exportTaskbar(const Json& state, std::string& toml)
{
    if (!state.is_object())
    {
        return;
    }
    toml += "\n[taskbar]\n";
    if (const auto alignment = json::intOr(state, "alignment", -1); alignment == 0 || alignment == 1)
    {
        toml += std::format("alignment = {}\n", tomlString(alignment == 0 ? "left" : "center"));
    }
    if (const auto taskView = json::intOr(state, "taskView", -1); taskView == 0 || taskView == 1)
    {
        toml += std::format("task_view = {}\n", taskView == 1);
    }
    if (const auto search = json::intOr(state, "search", -1); search >= 0 && search < 4)
    {
        toml += std::format("search = {}\n", tomlString(kSearchNames[static_cast<std::size_t>(search)]));
    }
    toml += std::format("autohide = {}\n", json::boolOr(state, "autohide", false));
}

void exportCursors(const Json& state, std::string& toml, ExportResult& result,
                   const std::function<std::string(std::string_view)>& extensionOf)
{
    if (!state.is_object())
    {
        return;
    }
    std::string map;
    for (std::size_t i = 0; i < domain::kCursorRoleCount; ++i)
    {
        const auto role = static_cast<domain::CursorRole>(i);
        const Json field = json::memberOr(state, domain::registryValueName(role), nullptr);
        const std::string copy = json::stringOr(field, "copy", "");
        const std::string source = copy.empty() ? json::stringOr(field, "data", "") : copy;
        if (source.empty())
        {
            continue;
        }
        const std::string extension = lower(extensionOf(source));
        if (extension != "cur" && extension != "ani")
        {
            result.notes.push_back(
                std::format("cursor '{}' is not a .cur/.ani file; skipped", domain::riceName(role)));
            continue;
        }
        const std::string file = std::format("{}.{}", domain::riceName(role), extension);
        result.files.push_back({source, "assets/cursors/" + file});
        map += std::format("{} = {}\n", domain::riceName(role), tomlString(file));
    }
    if (map.empty())
    {
        return;
    }
    toml += "\n[cursors]\nsource = \"assets/cursors\"\n";
    if (const auto size = json::intOr(state, "size", 0); size >= 32 && size <= 256 && size % 16 == 0)
    {
        toml += std::format("size = {}\n", size);
    }
    toml += "\n[cursors.map]\n" + map;
}

void exportSounds(const Json& state, std::string& toml, ExportResult& result,
                  const std::function<std::string(std::string_view)>& extensionOf)
{
    if (!state.is_object())
    {
        return;
    }
    std::string events;
    bool allSilent = true;
    bool anyField = false;
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        const auto event = static_cast<domain::SoundEvent>(i);
        const Json field = json::memberOr(state, domain::riceName(event), nullptr);
        if (!field.is_object())
        {
            continue;
        }
        anyField = true;
        const std::string copy = json::stringOr(field, "copy", "");
        const std::string source = copy.empty() ? json::stringOr(field, "data", "") : copy;
        if (source.empty())
        {
            continue;
        }
        allSilent = false;
        // Windows' own sounds are Windows' to give: an imported rice keeps the other computer's.
        if (json::boolOr(field, "keep", false))
        {
            continue;
        }
        if (lower(extensionOf(source)) != "wav")
        {
            result.notes.push_back(std::format("sound '{}' is not a .wav file; skipped", domain::riceName(event)));
            continue;
        }
        const std::string file = std::format("assets/sounds/{}.wav", domain::riceName(event));
        result.files.push_back({source, file});
        events += std::format("{} = {}\n", domain::riceName(event), tomlString(file));
    }
    if (anyField && allSilent && json::stringOr(json::memberOr(state, "scheme", nullptr), "data", "") == ".None")
    {
        toml += "\n[sounds]\nscheme = \"none\"\n";
        return;
    }
    if (!events.empty())
    {
        toml += "\n[sounds]\n\n[sounds.events]\n" + events;
    }
}

// An icon location ("C:\x.ico,0") that is an .ico of the user's (not Windows'): its file.
std::string userIconFile(const std::string& location,
                         const std::function<std::string(std::string_view)>& extensionOf)
{
    const auto comma = location.rfind(',');
    std::string file = comma == std::string::npos ? location : location.substr(0, comma);
    const std::string lowered = lower(file);
    if (file.empty() || lowered.starts_with("%systemroot%") || lowered.starts_with("%windir%") ||
        lowered.starts_with(R"(c:\windows\)") || lower(extensionOf(file)) != "ico")
    {
        return {};
    }
    return file;
}

// A captured icon: its copy in the store when it has one (the same picture, always there), else
// the location itself.
std::string capturedIcon(const Json& holder, std::string_view key)
{
    if (holder.is_string())
    {
        return holder.get<std::string>();
    }
    const std::string copy = json::stringOr(holder, "copy", "");
    return copy.empty() ? json::stringOr(holder, key, "") : copy;
}

void exportIcons(const Json& state, std::string& toml, ExportResult& result,
                 const std::function<std::string(std::string_view)>& extensionOf)
{
    if (!state.is_object())
    {
        return;
    }
    std::string desktop;
    for (std::size_t i = 0; i < domain::kDesktopIconCount; ++i)
    {
        const std::string name(domain::riceName(static_cast<domain::DesktopIcon>(i)));
        const std::string file = userIconFile(capturedIcon(json::memberOr(state, name, nullptr), "data"), extensionOf);
        if (!file.empty())
        {
            result.files.push_back({file, "assets/icons/" + name + ".ico"});
            desktop += std::format("{} = {}\n", name, tomlString("assets/icons/" + name + ".ico"));
        }
    }
    std::string folders;
    for (std::size_t i = 0; i < domain::kUserFolderCount; ++i)
    {
        const std::string name(domain::riceName(static_cast<domain::UserFolder>(i)));
        const Json field = json::memberOr(state, "folder:" + name, nullptr);
        const std::string file = userIconFile(capturedIcon(field, "data"), extensionOf);
        if (!file.empty())
        {
            const std::string relative = "assets/icons/folder-" + name + ".ico";
            result.files.push_back({file, relative});
            folders += std::format("{} = {}\n", name, tomlString(relative));
        }
    }
    std::string apps;
    std::vector<std::string> seen;
    Json shortcuts = json::memberOr(json::memberOr(state, "apps", Json::object()), "byShortcut", Json::object());
    if (!shortcuts.is_object())
    {
        shortcuts = Json::object();
    }
    for (const auto& [path, shortcut] : shortcuts.items())
    {
        const std::string program = json::stringOr(shortcut, "program", "");
        const std::string file = userIconFile(capturedIcon(shortcut, "icon"), extensionOf);
        if (file.empty() || !domain::isValidProgramName(program) || std::ranges::find(seen, program) != seen.end())
        {
            continue;
        }
        seen.push_back(program);
        const std::string relative = "assets/icons/app-" + program.substr(0, program.size() - 4) + ".ico";
        result.files.push_back({file, relative});
        apps += std::format("{} = {}\n", tomlString(program), tomlString(relative));
    }
    if (desktop.empty() && folders.empty() && apps.empty())
    {
        return;
    }
    toml += "\n[icons]\n" + desktop;
    if (!folders.empty())
    {
        toml += "\n[icons.folders]\n" + folders;
    }
    if (!apps.empty())
    {
        toml += "\n[icons.apps]\n" + apps;
    }
}

void exportLockScreen(const Json& state, std::string& toml, ExportResult& result,
                      const std::function<std::string(std::string_view)>& extensionOf)
{
    if (!state.is_object())
    {
        return;
    }
    std::string section;
    const Json picture = json::memberOr(state, "picture", Json::object());
    const Json spotlight = json::memberOr(picture, "spotlight", nullptr);
    if (spotlight.is_boolean() && spotlight.get<bool>())
    {
        section += "spotlight = true\n";
    }
    else
    {
        // The store's copy when there is one (always there); Windows' own pictures stay out.
        std::string file = json::stringOr(picture, "copy", "");
        if (file.empty())
        {
            file = json::stringOr(picture, "file", "");
        }
        const std::string extension = lower(extensionOf(file));
        if (!file.empty() && !lower(file).starts_with(R"(c:\windows\)") &&
            (extension == "jpg" || extension == "jpeg" || extension == "png"))
        {
            const std::string relative = std::string("assets/lockscreen.") + (extension == "png" ? "png" : "jpg");
            result.files.push_back({file, relative});
            section += std::format("image = {}\n", tomlString(relative));
        }
    }
    if (const Json tips = json::memberOr(state, "tips", nullptr); tips.is_boolean())
    {
        section += std::format("tips = {}\n", tips.get<bool>());
    }
    if (!section.empty())
    {
        toml += "\n[lockscreen]\n" + section;
    }
}

void exportWallpaper(const Json& state, const std::vector<ports::MonitorInfo>& monitors, std::string& toml,
                     ExportResult& result, const std::function<std::string(std::string_view)>& extensionOf)
{
    if (!state.is_object())
    {
        return;
    }
    const std::string background = json::stringOr(state, "background", "");
    const auto position = json::intOr(state, "position", 4);
    const std::string_view fit =
        position >= 0 && position < 5 ? kFitNames[static_cast<std::size_t>(position)] : "fill";

    std::string section = "\n[wallpaper]\n";
    if (domain::Color::parse(background))
    {
        section += std::format("background = {}\n", tomlString(background));
    }

    const Json slideshow = json::memberOr(state, "slideshow", nullptr);
    if (slideshow.is_object())
    {
        std::size_t index = 0;
        for (const auto& item : json::memberOr(slideshow, "items", Json::array()))
        {
            const std::string source = item.is_string() ? item.get<std::string>() : std::string();
            const std::string extension = source.empty() ? std::string() : lower(extensionOf(source));
            if (extension.empty())
            {
                continue;
            }
            result.files.push_back(
                {source, std::format("assets/wallpaper/slideshow/{:04}.{}", ++index, extension)});
        }
        if (index == 0)
        {
            result.notes.emplace_back("the slideshow has no exportable images; wallpaper skipped");
            return;
        }
        const auto interval =
            std::clamp<std::int64_t>(json::intOr(slideshow, "intervalMs", 1'800'000) / 1000, 10, 86'400);
        section +=
            std::format("\n[[wallpaper.monitor]]\nmatch = \"all\"\nkind = \"slideshow\"\n"
                        "source = \"assets/wallpaper/slideshow\"\ninterval = {}\nshuffle = {}\nfit = {}\n",
                        interval, json::boolOr(slideshow, "shuffle", false), tomlString(fit));
        toml += section;
        return;
    }

    // One file per distinct image; monitors are pinned by their identity key (a personal rice).
    std::map<std::string, std::string> relativeBySource;
    std::string rules;
    std::size_t images = 0;
    for (const auto& monitor : monitors)
    {
        const Json field =
            json::memberOr(state, std::string(kMonitorFieldPrefix) + monitor.identityKey, nullptr);
        if (!field.is_object())
        {
            continue;
        }
        const std::string source = json::stringOr(field, "file", "");
        if (source.empty())
        {
            if (domain::Color::parse(background))
            {
                rules += std::format("\n[[wallpaper.monitor]]\nmatch = {}\nkind = \"color\"\ncolor = {}\n",
                                     tomlString("edid:" + monitor.identityKey), tomlString(background));
            }
            continue;
        }
        auto known = relativeBySource.find(source);
        if (known == relativeBySource.end())
        {
            const std::string extension = lower(extensionOf(source));
            if (extension.empty())
            {
                result.notes.push_back(
                    std::format("the wallpaper of {} is not an image file; skipped", monitor.displayName));
                continue;
            }
            const std::string relative = std::format("assets/wallpaper/{}.{}", ++images, extension);
            result.files.push_back({source, relative});
            known = relativeBySource.emplace(source, relative).first;
        }
        if (position == 5) // span: one image across every monitor
        {
            section += std::format("mode = \"span\"\nsource = {}\n", tomlString(known->second));
            toml += section;
            return;
        }
        rules += std::format("\n[[wallpaper.monitor]]\nmatch = {}\nkind = \"image\"\nsource = {}\nfit = {}\n",
                             tomlString("edid:" + monitor.identityKey), tomlString(known->second),
                             tomlString(fit));
    }
    toml += section + rules;
}

} // namespace

std::string tomlString(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (byte < 0x20 || byte == 0x7F)
            {
                out += std::format("\\u{:04X}", byte);
            }
            else
            {
                out += c;
            }
        }
    }
    return out + "\"";
}

Result<ExportResult> exportRice(std::string_view name, const CapturedLook& look,
                                const std::function<std::string(std::string_view)>& extensionOf)
{
    if (name.empty() || name.size() > 128)
    {
        return fail(ErrorCode::InvalidArgument, "the exported rice needs a name (1..128 bytes)");
    }
    ExportResult result;
    result.toml = std::format("# Exported by Ricing Manager {} from this computer.\n"
                              "# Monitors are matched by their identity (edid:...): edit the rules\n"
                              "# (primary, left, all...) before sharing it.\n"
                              "schema = 1\n\n[meta]\nname = {}\nversion = \"1.0.0\"\n",
                              kVersion, tomlString(name));
    exportAppearance(look.appearance, result.toml, result.notes);
    exportTaskbar(look.taskbar, result.toml);
    exportCursors(look.cursors, result.toml, result, extensionOf);
    exportSounds(look.sounds, result.toml, result, extensionOf);
    exportIcons(look.icons, result.toml, result, extensionOf);
    exportLockScreen(look.lockscreen, result.toml, result, extensionOf);
    exportWallpaper(look.wallpaper, look.monitors, result.toml, result, extensionOf);
    return result;
}

} // namespace rmw::modules
