#include "PluginTools.hpp"

#include "rmw/bar/WidgetSource.hpp"
#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/plugins/PluginFiles.hpp"
#include "rmw/scripting/Runtime.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>

namespace rmw::cli
{
namespace
{

#include "PluginTemplate.inc"

std::string text(const std::filesystem::path& path)
{
    return utf16To8Lossy(path.native());
}

Result<void> writeText(const std::filesystem::path& file, std::string_view content)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return platform::writeFileAtomic(file, std::as_bytes(std::span(content)));
}

// ---- templates ------------------------------------------------------------------------------------

constexpr std::string_view kActionToml = R"(schema = 1

[plugin]
id          = "{id}"
name        = "{name}"
version     = "0.1.0"
api         = "^1.0"
authors     = ["you"]
license     = "MIT"
description = "An action for hotkeys, rules and the CLI: rmw action {id}:toggle"
kind        = ["action"]

[permissions]
actions = ["bar.toggle"]

[entry]
script = "main.luau"

[[action]]
name  = "toggle"
label = "Show or hide the bar"
)";

constexpr std::string_view kActionLuau = R"(--!strict
rmw.onaction("toggle", function(args)
    rmw.action("bar.toggle")
end)
)";

constexpr std::string_view kRuleToml = R"(schema = 1

[plugin]
id          = "{id}"
name        = "{name}"
version     = "0.1.0"
api         = "^1.0"
authors     = ["you"]
license     = "MIT"
description = "Dark mode while on battery."
kind        = ["rule"]

[permissions]
providers = ["battery"]
actions   = ["appearance.dark", "appearance.light"]

[entry]
script = "main.luau"
)";

constexpr std::string_view kRuleLuau = R"(--!strict
local wasPlugged: boolean? = nil

rmw.on("battery", function(battery)
    if not battery.present or battery.plugged_in == wasPlugged then
        return
    end
    wasPlugged = battery.plugged_in
    rmw.action(if battery.plugged_in then "appearance.light" else "appearance.dark")
end)
)";

constexpr std::string_view kTweakToml = R"(schema = 1

[plugin]
id          = "{id}"
name        = "{name}"
version     = "0.1.0"
api         = "^1.0"
authors     = ["you"]
license     = "MIT"
description = "Shows file extensions in Explorer."
kind        = ["tweak"]

# Only HKEY_CURRENT_USER keys of an allow-list (docs/plugins/tweaks.md); undone like any change.
[[tweak]]
key   = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
value = "HideFileExt"
type  = "dword"
data  = 0
restart_explorer = true
)";

constexpr std::string_view kSkinToml = R"(schema = 1

[plugin]
id          = "{id}"
name        = "{name}"
version     = "0.1.0"
api         = "^1.0"
authors     = ["you"]
license     = "MIT"
description = "A floating, rounded bar."
kind        = ["bar.skin"]

# The keys of a [[bar]]: a rice's bar with skin = "{id}" starts from these.
[skin]
mode       = "float"
thickness  = 40
margin     = [8, 12]
radius     = 12
background = { kind = "acrylic", opacity = 0.6 }
left       = ["start", "windows"]
center     = ["clock"]
right      = ["volume", "battery"]
)";

std::string fill(std::string_view text, std::string_view id, std::string_view name)
{
    std::string out(text);
    const std::string quotedName = std::format("\"{}\"", name);
    // "{id}" and "{name}" in the templates here; the id and name in plugins/template's manifest.
    for (const auto& [key, value] : {std::pair<std::string_view, std::string_view>{"{id}", id},
                                     {"{name}", name},
                                     {"io.github.you.my-widget", id},
                                     {"\"My widget\"", quotedName}})
    {
        for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size()))
        {
            out.replace(at, key.size(), value);
        }
    }
    return out;
}

} // namespace

std::vector<std::string> describePermissions(const domain::PluginPermissions& permissions)
{
    std::vector<std::string> lines;
    for (const auto& name : permissions.providers)
    {
        const auto* capability = domain::findPluginProvider(name);
        lines.push_back(std::format("{} {}", capability != nullptr && capability->sensitive ? "!" : " ",
                                    capability != nullptr ? capability->summary : name));
    }
    for (const auto& name : permissions.actions)
    {
        const auto* capability = domain::findPluginAction(name);
        lines.push_back(std::format("{} {}", capability != nullptr && capability->sensitive ? "!" : " ",
                                    capability != nullptr ? capability->summary : name));
    }
    for (const auto& domain : permissions.network)
    {
        lines.push_back(std::format("! reach {} over the Internet", domain));
    }
    if (permissions.storage)
    {
        lines.push_back(
            std::format("  keep up to {} KB of its own data", limits::kPluginStorageMaxBytes >> 10));
    }
    return lines;
}

PluginCheck checkPlugin(const std::filesystem::path& folder)
{
    PluginCheck check;
    domain::Diagnostics diagnostics;
    auto loaded = plugins::readPluginManifest(folder, &diagnostics);
    for (const auto& diagnostic : diagnostics.all())
    {
        (diagnostic.severity == domain::Severity::Error ? check.errors : check.warnings)
            .push_back(diagnostic.toString("plugin.toml"));
    }
    if (!loaded)
    {
        if (check.errors.empty())
        {
            check.errors.push_back(loaded.error().message());
        }
        return check;
    }
    const domain::PluginManifest& manifest = loaded->manifest;
    if (manifest.hasScript())
    {
        auto source = plugins::readPluginText(folder / std::filesystem::path(utf8To16Lossy(manifest.script)),
                                              scripting::kScriptMaxBytes);
        if (!source)
        {
            check.errors.push_back(std::format("{}: {}", manifest.script, source.error().message()));
        }
        else if (auto compiled = scripting::checkScript(*source, manifest.script); !compiled)
        {
            check.errors.push_back(compiled.error().message());
        }
    }
    if (manifest.widget)
    {
        const auto& widget = *manifest.widget;
        auto source = plugins::readPluginText(folder / std::filesystem::path(utf8To16Lossy(widget.ui)),
                                              bar::kWidgetSourceMaxBytes);
        if (!source)
        {
            check.errors.push_back(std::format("{}: {}", widget.ui, source.error().message()));
        }
        else
        {
            bar::WidgetSourceOptions options;
            options.prefix = "Rmwp1";
            options.component = widget.component;
            options.verticalComponent = widget.verticalComponent;
            options.resolveImage = [&folder](std::string_view path) -> Result<std::string> {
                auto file = plugins::pluginImageFile(folder, path);
                if (!file)
                {
                    return std::unexpected(std::move(file.error()));
                }
                return utf16To8Lossy(file->generic_wstring());
            };
            auto checked = bar::checkWidgetSource(*source, widget.ui, options);
            if (!checked)
            {
                check.errors.push_back(checked.error().message());
            }
            else
            {
                const auto has = [](const std::vector<std::string>& names, std::string_view name) {
                    return std::ranges::find(names, name) != names.end();
                };
                for (const auto& property : widget.properties)
                {
                    if (!has(checked->interface.inProperties, property.name))
                    {
                        check.errors.push_back(std::format("{}: {} has no `in property` '{}'", widget.ui,
                                                           widget.component, property.name));
                    }
                }
                for (const auto& callback : widget.callbacks)
                {
                    if (!has(checked->interface.callbacks, callback.name))
                    {
                        check.errors.push_back(std::format("{}: {} has no callback '{}'", widget.ui,
                                                           widget.component, callback.name));
                    }
                }
                for (const std::string_view name : {"foreground", "vertical"})
                {
                    if (!has(checked->interface.inProperties, name))
                    {
                        check.warnings.push_back(std::format("{}: {} does not take `{}` from the bar",
                                                             widget.ui, widget.component, name));
                    }
                }
            }
        }
    }
    check.manifest = std::move(loaded->manifest);
    return check;
}

Result<void> newPlugin(std::string_view kind, const std::filesystem::path& folder, std::string_view id)
{
    std::error_code ec;
    if (std::filesystem::exists(folder, ec))
    {
        return fail(ErrorCode::AlreadyExists, std::format("'{}' exists already", text(folder)));
    }
    const std::string name = text(folder.filename());
    std::string pluginId(id);
    if (pluginId.empty())
    {
        std::string slug;
        for (const char c : name)
        {
            const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            slug += (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ? lower : '-';
        }
        while (slug.find("--") != std::string::npos)
        {
            slug.replace(slug.find("--"), 2, "-");
        }
        while (!slug.empty() && slug.front() == '-')
        {
            slug.erase(slug.begin());
        }
        while (!slug.empty() && slug.back() == '-')
        {
            slug.pop_back();
        }
        pluginId = "io.github.you." + (slug.empty() ? std::string("my-plugin") : slug);
    }
    if (!domain::isValidPackageId(pluginId))
    {
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is not a plugin id (io.github.you.name)", pluginId));
    }
    std::vector<std::pair<std::string, std::string>> files;
    if (kind == "widget")
    {
        files = {{"plugin.toml", fill(kTemplateToml, pluginId, name)},
                 {"main.luau", std::string(kTemplateLuau)},
                 {"widget.slint", std::string(kTemplateSlint)}};
    }
    else if (kind == "action")
    {
        files = {{"plugin.toml", fill(kActionToml, pluginId, name)}, {"main.luau", std::string(kActionLuau)}};
    }
    else if (kind == "rule")
    {
        files = {{"plugin.toml", fill(kRuleToml, pluginId, name)}, {"main.luau", std::string(kRuleLuau)}};
    }
    else if (kind == "tweak")
    {
        files = {{"plugin.toml", fill(kTweakToml, pluginId, name)}};
    }
    else if (kind == "skin")
    {
        files = {{"plugin.toml", fill(kSkinToml, pluginId, name)}};
    }
    else
    {
        return fail(ErrorCode::InvalidArgument, "the templates are widget, action, rule, tweak and skin");
    }
    if (kind == "widget" || kind == "action" || kind == "rule")
    {
        files.emplace_back("rmw.d.luau", std::string(kRmwTypes));
        files.emplace_back(".luaurc", std::string(kLuauRc));
        files.emplace_back(".vscode/settings.json", std::string(kVsCodeSettings));
        files.emplace_back(".vscode/extensions.json", std::string(kVsCodeExtensions));
    }
    for (const auto& [file, content] : files)
    {
        if (auto written = writeText(folder / file, content); !written)
        {
            return written;
        }
    }
    return {};
}

} // namespace rmw::cli
