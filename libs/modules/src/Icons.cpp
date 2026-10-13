#include "rmw/modules/Icons.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/CursorFiles.hpp"

#include <array>
#include <format>
#include <string>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr std::string_view kFolderPrefix = "folder:";
constexpr std::string_view kTypePrefix = "type:";
constexpr std::string_view kRecycleBinNow = "recycle-bin-now";

struct DesktopEntry
{
    domain::DesktopIcon icon;
    std::string_view clsid;
    std::string_view value; // under its DefaultIcon key; "" = the key's default value
};

// Order matches the DesktopIcon enumerators.
constexpr std::array<DesktopEntry, domain::kDesktopIconCount> kDesktop = {{
    {domain::DesktopIcon::ThisPc, "{20D04FE0-3AEA-1069-A2D8-08002B30309D}", ""},
    {domain::DesktopIcon::RecycleBin, "{645FF040-5081-101B-9F08-00AA002F954E}", "empty"},
    {domain::DesktopIcon::RecycleBinFull, "{645FF040-5081-101B-9F08-00AA002F954E}", "full"},
    {domain::DesktopIcon::Network, "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", ""},
    {domain::DesktopIcon::UserFiles, "{59031a47-3f72-44a7-89c5-5595fe6b30ee}", ""},
    {domain::DesktopIcon::ControlPanel, "{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}", ""},
}};

std::string clsidKey(std::string_view clsid)
{
    return std::format(R"(Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{})", clsid);
}

std::string defaultIconKey(std::string_view clsid)
{
    return clsidKey(clsid) + R"(\DefaultIcon)";
}

std::string classKey(std::string_view progId)
{
    return std::format(R"(Software\Classes\{})", progId);
}

Json stringField(const std::optional<ports::RegistryValue>& value)
{
    if (!value || (value->type != ports::RegistryValue::Type::String &&
                   value->type != ports::RegistryValue::Type::ExpandString))
    {
        return nullptr;
    }
    return Json{{"type", value->type == ports::RegistryValue::Type::String ? "string" : "expand"},
                {"data", value->text}};
}

// The location to write: the captured one while its file is there, else its copy in the store.
std::string chosen(ports::IIcons& icons, const std::string& location, const std::string& copy)
{
    return copy.empty() || location.empty() || icons.exists(location) ? location : copy;
}

Result<void> writeStringField(ports::IRegistry& registry, ports::IIcons& icons, const std::string& key,
                              std::string_view name, const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(key, name);
    }
    const auto type = value.find("type");
    const auto data = value.find("data");
    if (!value.is_object() || type == value.end() || data == value.end() || !type->is_string() ||
        !data->is_string())
    {
        return fail(ErrorCode::InvalidArgument, std::format("icon field '{}' is malformed", name));
    }
    const std::string text = chosen(icons, data->get<std::string>(), json::stringOr(value, "copy", ""));
    return registry.write(key, name,
                          *type == "string" ? ports::RegistryValue::fromString(text)
                                            : ports::RegistryValue::fromExpandString(text));
}

// A value as fieldEquivalent() compares it: no "copy", a folder's {"data": x} as x, a file type's
// icon alone (not the ProgID it went to).
Json comparable(const Json& value)
{
    if (!value.is_object())
    {
        return value;
    }
    if (value.contains("icon"))
    {
        return comparable(value["icon"]);
    }
    Json out = value;
    out.erase("copy");
    if (out.size() == 1 && out.contains("data"))
    {
        return out["data"];
    }
    if (const auto shortcuts = out.find("byShortcut"); shortcuts != out.end() && shortcuts->is_object())
    {
        for (auto& [path, shortcut] : shortcuts->items())
        {
            if (shortcut.is_object())
            {
                shortcut.erase("copy");
            }
        }
    }
    return out;
}

std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return text;
}

// "C:\x.ico,0" -> "C:\x.ico".
std::string iconFile(std::string_view location)
{
    const auto comma = location.rfind(',');
    return std::string(comma == std::string_view::npos ? location : location.substr(0, comma));
}

// The package's .ico, checked and in the store: "C:\...\store\<hash>.ico,0".
Result<std::string> storeIcon(engine::IAssetSink& assets, const std::string& file)
{
    const std::string context = std::format("icon '{}'", file);
    auto bytes = assets.readPackageFile(file, limits::kIconMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()).withContext(context));
    }
    if (auto valid = domain::validateIconFile(*bytes); !valid)
    {
        return std::unexpected(std::move(valid.error()).withContext(context));
    }
    auto stored = assets.fromPackage(file);
    if (!stored)
    {
        return std::unexpected(std::move(stored.error()));
    }
    const std::u8string path = stored->u8string();
    return std::string(path.begin(), path.end()) + ",0";
}

// What each program's shortcuts show: by program from a rice, by shortcut as captured.
bool sameApps(const Json& byProgram, const Json& byShortcut)
{
    const Json programs = json::memberOr(byProgram, "byProgram", Json::object());
    const Json shortcuts = json::memberOr(byShortcut, "byShortcut", Json::object());
    if (!programs.is_object() || !shortcuts.is_object())
    {
        return false;
    }
    for (const auto& [path, shortcut] : shortcuts.items())
    {
        const std::string program = json::stringOr(shortcut, "program", "");
        if (const auto wanted = programs.find(program); wanted != programs.end() && wanted->is_string() &&
                                                        json::stringOr(shortcut, "icon", "") != wanted->get<std::string>())
        {
            return false;
        }
    }
    return true;
}

// Two captures of the shortcuts: the same when every shortcut both know (same program) shows the same icon.
// A program installed or removed since is not a change of ours being undone: v1.8 took any new shortcut as
// the person's change, and a restore left every rice icon where it was.
bool sameShortcuts(const Json& a, const Json& b)
{
    if (!a.is_object() || !b.is_object())
    {
        return a == b;
    }
    for (const auto& [path, shortcut] : a.items())
    {
        const auto other = b.find(path);
        if (other == b.end() || json::stringOr(*other, "program", "") != json::stringOr(shortcut, "program", ""))
        {
            continue;
        }
        if (lower(json::stringOr(*other, "icon", "")) != lower(json::stringOr(shortcut, "icon", "")))
        {
            return false;
        }
    }
    return true;
}

} // namespace

Result<Json> IconsLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const
{
    Json desired = Json::object();
    if (!rice.icons)
    {
        return desired;
    }
    const domain::IconsConfig& config = *rice.icons;
    for (const auto& [icon, file] : config.desktop)
    {
        auto stored = storeIcon(assets, file);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        const Json field{{"type", "expand"}, {"data", *stored}};
        desired[std::string(domain::riceName(icon))] = field;
        if (icon == domain::DesktopIcon::RecycleBin)
        {
            desired[std::string(kRecycleBinNow)] = field; // Explorer switches it as the bin fills
        }
    }
    for (const auto& [folder, file] : config.folders)
    {
        auto stored = storeIcon(assets, file);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        desired[std::string(kFolderPrefix) + std::string(domain::riceName(folder))] = *stored;
    }
    if (!config.apps.empty())
    {
        Json programs = Json::object();
        for (const auto& [program, file] : config.apps)
        {
            auto stored = storeIcon(assets, file);
            if (!stored)
            {
                return std::unexpected(std::move(stored.error()));
            }
            programs[program] = *stored;
        }
        desired["apps"] = Json{{"byProgram", std::move(programs)}};
    }
    for (const auto& [extension, file] : config.types)
    {
        auto stored = storeIcon(assets, file);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        desired[std::string(kTypePrefix) + extension] = Json{{"icon", {{"type", "expand"}, {"data", *stored}}}};
    }
    return desired;
}

Result<void> IconsLogic::internalize(Json& state, engine::IAssetSink& assets) const
{
    // Only .ico files outside Windows: an .exe or a .dll an icon comes from stays where it is. The
    // copy goes next to the location (`holder["copy"]`), which stays as it is.
    const auto keep = [&](const std::string& location, Json& holder) {
        const std::string file = iconFile(location);
        const std::string lowered = lower(file);
        if (file.empty() || !lowered.ends_with(".ico") || lowered.starts_with("%systemroot%") ||
            lowered.starts_with("%windir%") || lowered.starts_with(R"(c:\windows\)") ||
            file.find('%') != std::string::npos)
        {
            return;
        }
        auto wide = utf8To16(file);
        if (!wide)
        {
            return;
        }
        auto stored = assets.fromSystem(std::filesystem::path(*wide), "ico");
        if (!stored)
        {
            log::warn("icons: cannot keep a copy of {}: {}", file, stored.error().message());
            return;
        }
        const std::u8string path = stored->u8string();
        holder["copy"] = std::string(path.begin(), path.end()) + location.substr(file.size());
    };
    for (auto& [name, value] : state.items())
    {
        if (value.is_string() && name.starts_with(kFolderPrefix))
        {
            Json folder{{"data", value.get<std::string>()}};
            keep(folder["data"].get<std::string>(), folder);
            if (folder.contains("copy"))
            {
                value = std::move(folder);
            }
        }
        else if (value.is_object() && value.contains("data") && value["data"].is_string())
        {
            keep(value["data"].get<std::string>(), value);
        }
        else if (name.starts_with(kTypePrefix) && value.is_object() && value.contains("icon") &&
                 value["icon"].is_object() && value["icon"].contains("data") && value["icon"]["data"].is_string())
        {
            Json& icon = value["icon"];
            keep(icon["data"].get<std::string>(), icon);
        }
        else if (name == "apps" && value.contains("byShortcut") && value["byShortcut"].is_object())
        {
            for (auto& [path, shortcut] : value["byShortcut"].items())
            {
                if (shortcut.contains("icon") && shortcut["icon"].is_string())
                {
                    keep(shortcut["icon"].get<std::string>(), shortcut);
                }
            }
        }
    }
    return {};
}

bool IconsLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    if (field == "apps")
    {
        if (a.contains("byProgram") && b.contains("byShortcut"))
        {
            return sameApps(a, b);
        }
        if (b.contains("byProgram") && a.contains("byShortcut"))
        {
            return sameApps(b, a);
        }
        if (a.contains("byShortcut") && b.contains("byShortcut"))
        {
            return sameShortcuts(a["byShortcut"], b["byShortcut"]);
        }
    }
    return comparable(a) == comparable(b);
}

Result<engine::CaptureResult> IconsSystem::capture()
{
    engine::CaptureResult result;
    for (const auto& entry : kDesktop)
    {
        auto value = m_registry.read(defaultIconKey(entry.clsid), entry.value);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[std::string(domain::riceName(entry.icon))] = stringField(*value);
    }
    auto now = m_registry.read(defaultIconKey(kDesktop[1].clsid), "");
    if (!now)
    {
        return std::unexpected(std::move(now.error()));
    }
    result.state[std::string(kRecycleBinNow)] = stringField(*now);
    for (std::size_t i = 0; i < domain::kUserFolderCount; ++i)
    {
        const std::string_view name = domain::riceName(static_cast<domain::UserFolder>(i));
        auto icon = m_icons.folderIcon(name);
        if (!icon)
        {
            return std::unexpected(std::move(icon.error()));
        }
        result.state[std::string(kFolderPrefix) + std::string(name)] = *icon ? Json(**icon) : Json(nullptr);
    }
    auto shortcuts = m_icons.shortcuts();
    if (!shortcuts)
    {
        return std::unexpected(std::move(shortcuts.error()));
    }
    Json byShortcut = Json::object();
    for (const auto& shortcut : *shortcuts)
    {
        if (!shortcut.program.empty())
        {
            byShortcut[shortcut.path] = Json{{"program", shortcut.program}, {"icon", shortcut.icon}};
        }
    }
    result.state["apps"] = Json{{"byShortcut", std::move(byShortcut)}};
    return result;
}

Result<engine::CaptureResult> IconsSystem::captureFields(std::span<const std::string> fields)
{
    auto result = capture();
    if (!result)
    {
        return result;
    }
    for (const std::string& field : fields)
    {
        if (!field.starts_with(kTypePrefix) || !domain::isValidFileExtension(field.substr(kTypePrefix.size())))
        {
            continue;
        }
        auto progId = m_icons.progIdOf(field.substr(kTypePrefix.size()));
        if (!progId)
        {
            return std::unexpected(std::move(progId.error()));
        }
        Json type{{"progId", *progId}, {"icon", nullptr}};
        if (!progId->empty())
        {
            auto value = m_registry.read(classKey(*progId) + R"(\DefaultIcon)", "");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            type["icon"] = stringField(*value);
        }
        result->state[field] = std::move(type);
    }
    return result;
}

Result<void> IconsSystem::apply(const Json& desired)
{
    bool changed = false;
    for (const auto& entry : kDesktop)
    {
        if (const auto value = desired.find(domain::riceName(entry.icon)); value != desired.end())
        {
            if (auto written =
                    writeStringField(m_registry, m_icons, defaultIconKey(entry.clsid), entry.value, *value);
                !written)
            {
                return written;
            }
            changed = true;
        }
    }
    if (const auto value = desired.find(kRecycleBinNow); value != desired.end())
    {
        if (auto written = writeStringField(m_registry, m_icons, defaultIconKey(kDesktop[1].clsid), "", *value);
            !written)
        {
            return written;
        }
        changed = true;
    }
    for (std::size_t i = 0; i < domain::kUserFolderCount; ++i)
    {
        const std::string name(domain::riceName(static_cast<domain::UserFolder>(i)));
        if (const auto value = desired.find(std::string(kFolderPrefix) + name); value != desired.end())
        {
            const bool held = value->is_object() && value->contains("data") && (*value)["data"].is_string();
            if (!value->is_null() && !value->is_string() && !held)
            {
                return fail(ErrorCode::InvalidArgument, std::format("icon field 'folder:{}' is malformed", name));
            }
            std::optional<std::string> icon;
            if (value->is_string())
            {
                icon = value->get<std::string>();
            }
            else if (held)
            {
                icon = chosen(m_icons, (*value)["data"].get<std::string>(), json::stringOr(*value, "copy", ""));
            }
            if (auto set = m_icons.setFolderIcon(name, icon); !set)
            {
                return set;
            }
            changed = true;
        }
    }
    if (const auto apps = desired.find("apps"); apps != desired.end())
    {
        auto shortcuts = m_icons.shortcuts();
        if (!shortcuts)
        {
            return std::unexpected(std::move(shortcuts.error()));
        }
        const Json programs = json::memberOr(*apps, "byProgram", Json());
        const Json byShortcut = json::memberOr(*apps, "byShortcut", Json());
        if (!programs.is_object() && !byShortcut.is_object())
        {
            return fail(ErrorCode::InvalidArgument, "icon field 'apps' is malformed");
        }
        for (const auto& shortcut : *shortcuts)
        {
            std::optional<std::string> wanted;
            if (programs.is_object())
            {
                if (const auto icon = programs.find(shortcut.program); icon != programs.end() && icon->is_string())
                {
                    wanted = icon->get<std::string>();
                }
            }
            else if (const auto entry = byShortcut.find(shortcut.path);
                     entry != byShortcut.end() && json::stringOr(*entry, "program", "") == shortcut.program)
            {
                // A shortcut that now points elsewhere is someone else's: left alone.
                wanted = chosen(m_icons, json::stringOr(*entry, "icon", ""), json::stringOr(*entry, "copy", ""));
            }
            if (wanted && *wanted != shortcut.icon)
            {
                // One shortcut that cannot be written (read-only, in the cloud only) is not a reason to undo
                // the whole rice: it keeps its icon, and the log says which.
                if (auto set = m_icons.setShortcutIcon(shortcut.path, *wanted); !set)
                {
                    log::warn("icons: {} keeps its icon: {}", shortcut.path, set.error().toString());
                    continue;
                }
                changed = true;
            }
        }
    }
    for (const auto& [field, value] : desired.items())
    {
        if (!field.starts_with(kTypePrefix))
        {
            continue;
        }
        const std::string extension = field.substr(kTypePrefix.size());
        const auto icon = value.is_object() ? value.find("icon") : value.end();
        if (!domain::isValidFileExtension(extension) || !value.is_object() || icon == value.end())
        {
            return fail(ErrorCode::InvalidArgument, std::format("icon field '{}' is malformed", field));
        }
        // The ProgID it was captured with (a restore), else the one Windows uses now.
        std::string progId = json::stringOr(value, "progId", "");
        if (progId.empty())
        {
            auto found = m_icons.progIdOf(extension);
            if (!found)
            {
                return std::unexpected(std::move(found.error()));
            }
            progId = std::move(*found);
        }
        if (progId.empty())
        {
            log::warn("icons: no program opens .{} files; its icon is left alone", extension);
            continue;
        }
        const std::string key = classKey(progId) + R"(\DefaultIcon)";
        if (auto written = writeStringField(m_registry, m_icons, key, "", *icon); !written)
        {
            return written;
        }
        changed = true;
    }
    // A restore takes values off keys that were not there before: those go too.
    for (const auto& entry : kDesktop)
    {
        if (const auto value = desired.find(domain::riceName(entry.icon)); value != desired.end() && value->is_null())
        {
            [[maybe_unused]] auto icon = m_registry.removeKeyIfEmpty(defaultIconKey(entry.clsid));
            [[maybe_unused]] auto clsid = m_registry.removeKeyIfEmpty(clsidKey(entry.clsid));
        }
    }
    for (const auto& [field, value] : desired.items())
    {
        if (field.starts_with(kTypePrefix) && json::memberOr(value, "icon", Json()).is_null())
        {
            const std::string progId = json::stringOr(value, "progId", "");
            if (!progId.empty())
            {
                [[maybe_unused]] auto icon = m_registry.removeKeyIfEmpty(classKey(progId) + R"(\DefaultIcon)");
                [[maybe_unused]] auto type = m_registry.removeKeyIfEmpty(classKey(progId));
            }
        }
    }
    if (changed)
    {
        m_icons.refresh();
    }
    return {};
}

} // namespace rmw::modules
