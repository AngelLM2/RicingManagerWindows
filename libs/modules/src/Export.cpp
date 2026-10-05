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
        const std::string source = json::stringOr(field, "data", "");
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
    exportWallpaper(look.wallpaper, look.monitors, result.toml, result, extensionOf);
    return result;
}

} // namespace rmw::modules
