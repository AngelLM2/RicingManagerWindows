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

bool isReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// ---- templates ------------------------------------------------------------------------------------

constexpr std::string_view kWidgetToml = R"(schema = 1

[plugin]
id          = "{id}"
name        = "{name}"
version     = "0.1.0"
api         = "^1.0"
authors     = ["you"]
license     = "MIT"
description = "A widget for the Ricing Manager bar."
kind        = ["bar.widget"]

[permissions]
providers = ["system.cpu"]

[entry]
script = "main.luau"

[widget]
ui        = "widget.slint"
component = "MyWidget"

[widget.properties]
text = "string"

[widget.callbacks]
clicked = []

[config]
prefix = { type = "string", default = "CPU ", label = "Text before the number" }
)";

constexpr std::string_view kWidgetLuau = R"(--!strict
-- Runs in rmw-plugin-host, sandboxed: see docs/plugins and rmw.d.luau. Save the file and the
-- widget reloads (rmw plugin dev .).

local widgets = {}

rmw.widget(function(w)
    widgets[w.id] = w
    w:set("text", w.config.prefix .. "…")
    w:on("clicked", function()
        rmw.log("clicked on " .. w.id)
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("system.cpu", function(cpu)
    for _, w in widgets do
        w:set("text", string.format("%s%d%%", w.config.prefix, math.floor(cpu.usage + 0.5)))
    end
end)
)";

constexpr std::string_view kWidgetSlint =
    R"(// The widget's look. The bar gives every widget `foreground` and `vertical`; the rest is yours
// ([widget.properties] and [widget.callbacks] in plugin.toml).
export component MyWidget inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <string> text: "…";
    callback clicked();
    min-width: label.preferred-width + 16px;
    min-height: 32px;
    border-radius: 6px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    label := Text {
        text: root.text;
        color: root.foreground;
        font-size: 12px;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}
)";

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
    for (const auto& [key, value] :
         {std::pair{std::string_view("{id}"), id}, std::pair{std::string_view("{name}"), name}})
    {
        for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size()))
        {
            out.replace(at, key.size(), value);
        }
    }
    return out;
}

// ---- zip (store, no compression: what a .rmwp needs) ---------------------------------------------

std::uint32_t crc32(std::span<const std::byte> bytes)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : bytes)
    {
        crc = table[(crc ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

void put16(std::vector<std::byte>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void put32(std::vector<std::byte>& out, std::uint32_t value)
{
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

void putText(std::vector<std::byte>& out, std::string_view text)
{
    for (const char c : text)
    {
        out.push_back(static_cast<std::byte>(c));
    }
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
        files = {{"plugin.toml", fill(kWidgetToml, pluginId, name)},
                 {"main.luau", std::string(kWidgetLuau)},
                 {"widget.slint", std::string(kWidgetSlint)}};
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
    for (const auto& [file, content] : files)
    {
        if (auto written = writeText(folder / file, content); !written)
        {
            return written;
        }
    }
    return {};
}

Result<std::uint64_t> packPlugin(const std::filesystem::path& folder, const std::filesystem::path& out)
{
    auto root = platform::finalPath(folder);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(*root, ec); !ec && it != decltype(it){};
         it.increment(ec))
    {
        const std::string name = text(it->path().filename());
        if (name.starts_with('.') || it->path().extension() == L".rmwp")
        {
            if (it->is_directory(ec))
            {
                it.disable_recursion_pending(); // .git, .vscode...
            }
            continue;
        }
        if (isReparsePoint(it->path()))
        {
            return fail(ErrorCode::AccessDenied, std::format("{}: links are not allowed in a plugin", name));
        }
        if (it->is_directory(ec))
        {
            continue;
        }
        const std::string relative =
            utf16To8Lossy(std::filesystem::relative(it->path(), *root, ec).generic_wstring());
        auto valid = domain::validatePackagePath(relative);
        if (!valid)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: {}", relative, valid.error().message()));
        }
        files.emplace_back(std::move(*valid), it->path());
    }
    if (ec)
    {
        return fail(ErrorCode::Io, ec.message());
    }
    if (files.size() > limits::kPackageMaxEntries)
    {
        return fail(ErrorCode::LimitExceeded, std::format("more than {} files", limits::kPackageMaxEntries));
    }
    std::ranges::sort(files);
    std::vector<std::byte> zip;
    std::vector<std::byte> directory;
    constexpr std::uint32_t kUtf8Names = 0x0800;
    constexpr std::uint32_t kDosDate = 0x0021; // 1980-01-01: the same files make the same package
    for (const auto& [name, path] : files)
    {
        auto bytes = platform::readFileLimited(path, std::uint64_t{64} << 20);
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()).withContext(name));
        }
        const std::uint32_t crc = crc32(*bytes);
        const auto size = static_cast<std::uint32_t>(bytes->size());
        const auto offset = static_cast<std::uint32_t>(zip.size());
        put32(zip, 0x04034b50);
        put16(zip, 10);
        put16(zip, kUtf8Names);
        put16(zip, 0); // stored
        put16(zip, 0);
        put16(zip, kDosDate);
        put32(zip, crc);
        put32(zip, size);
        put32(zip, size);
        put16(zip, static_cast<std::uint32_t>(name.size()));
        put16(zip, 0);
        putText(zip, name);
        zip.insert(zip.end(), bytes->begin(), bytes->end());

        put32(directory, 0x02014b50);
        put16(directory, 20);
        put16(directory, 10);
        put16(directory, kUtf8Names);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, kDosDate);
        put32(directory, crc);
        put32(directory, size);
        put32(directory, size);
        put16(directory, static_cast<std::uint32_t>(name.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, offset);
        putText(directory, name);
        if (zip.size() > limits::kPackageMaxTotalBytes / 16)
        {
            return fail(ErrorCode::LimitExceeded, "the plugin is larger than 64 MB");
        }
    }
    const auto directoryOffset = static_cast<std::uint32_t>(zip.size());
    zip.insert(zip.end(), directory.begin(), directory.end());
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<std::uint32_t>(files.size()));
    put16(zip, static_cast<std::uint32_t>(files.size()));
    put32(zip, static_cast<std::uint32_t>(directory.size()));
    put32(zip, directoryOffset);
    put16(zip, 0);
    if (auto written = platform::writeFileAtomic(out, zip); !written)
    {
        return std::unexpected(std::move(written.error()));
    }
    return zip.size();
}

} // namespace rmw::cli
