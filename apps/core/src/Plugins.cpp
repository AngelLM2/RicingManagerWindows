#include "Plugins.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/json/Value.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <system_error>

namespace rmw::core
{
namespace
{

using json::Json;

constexpr std::string_view kDevMeta = "plugin_dev";           // ["<folder>", ...]
constexpr std::string_view kApprovedMeta = "plugin_approved"; // {"<id>": permissions}
constexpr std::string_view kEnabledMeta = "plugin_enabled";   // ["<id>", ...]
constexpr std::string_view kDisabledMeta = "plugin_disabled"; // {"<id>": "<why>"}
constexpr std::string_view kRiceMeta = "rice_plugins";        // {"required": [...], "settings": {...}}

constexpr std::size_t kMaxDevFolders = 32;
constexpr std::uint64_t kInstallMaxBytes = 64ull << 20;
constexpr std::size_t kInstallMaxFiles = limits::kPackageMaxEntries;

std::string text(const std::filesystem::path& path)
{
    return utf16To8Lossy(path.native());
}

Result<std::string> readText(const std::filesystem::path& file, std::uint64_t maxBytes)
{
    auto bytes = platform::readFileLimited(file, maxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// The list without the items `drop` picks (nlohmann's arrays have no erase_if).
template <class Drop>
Json without(const Json& list, Drop drop)
{
    Json kept = Json::array();
    for (const auto& item : list)
    {
        if (!drop(item))
        {
            kept.push_back(item);
        }
    }
    return kept;
}

bool isText(const Json& item, std::string_view value)
{
    return item.is_string() && item.get<std::string>() == value;
}

// A link, a junction or any other reparse point: never followed while copying a plugin.
bool isReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool contains(const Json& list, std::string_view value)
{
    return list.is_array() && std::ranges::any_of(list, [&](const Json& item) {
               return item.is_string() && item.get<std::string>() == value;
           });
}

} // namespace

const domain::Value* RicePlugins::settingsOf(std::string_view id) const noexcept
{
    for (const auto& [plugin, value] : settings)
    {
        if (plugin == id)
        {
            return &value;
        }
    }
    return nullptr;
}

bool PluginEntry::needsApproval() const
{
    if (!manifest)
    {
        return false;
    }
    const auto& asked = manifest->permissions;
    const bool asksNothing =
        asked.providers.empty() && asked.actions.empty() && asked.network.empty() && !asked.storage;
    return !asksNothing && (!approved || !asked.isWithin(*approved));
}

Json PluginEntry::toJson() const
{
    Json entry{{"id", id},
               {"folder", text(folder)},
               {"dev", dev},
               {"enabled", enabled},
               {"needsApproval", needsApproval()}};
    if (!disabled.empty())
    {
        entry["disabled"] = disabled;
    }
    if (!problem.empty())
    {
        entry["problem"] = problem;
    }
    if (manifest)
    {
        Json kinds = Json::array();
        for (const auto kind : manifest->kinds)
        {
            kinds.push_back(domain::toString(kind));
        }
        Json actions = Json::array();
        for (const auto& action : manifest->actions)
        {
            actions.push_back({{"name", std::format("{}:{}", id, action.name)}, {"label", action.label}});
        }
        Json tweaks = Json::array();
        for (const auto& tweak : manifest->tweaks)
        {
            tweaks.push_back({{"key", tweak.key}, {"value", tweak.value}});
        }
        entry["name"] = manifest->name;
        entry["version"] = manifest->version.toString();
        entry["kinds"] = std::move(kinds);
        entry["permissions"] = plugins::permissionsToJson(manifest->permissions);
        entry["sensitive"] = manifest->permissions.hasSensitive();
        entry["actions"] = std::move(actions);
        entry["tweaks"] = std::move(tweaks);
        entry["description"] = manifest->description;
    }
    return entry;
}

PluginManager::PluginManager(std::filesystem::path dataDirectory, engine::StateDb& state)
    : m_data(std::move(dataDirectory))
    , m_state(state)
{
}

Json PluginManager::metaJson(std::string_view key, Json fallback)
{
    auto stored = m_state.meta(key);
    if (!stored || !*stored)
    {
        return fallback;
    }
    auto parsed = json::parse(**stored);
    return parsed && parsed->type() == fallback.type() ? std::move(*parsed) : fallback;
}

void PluginManager::setMetaJson(std::string_view key, const Json& value)
{
    const std::string dumped = json::dump(value);
    if (auto saved = m_state.setMeta(key, dumped); !saved)
    {
        log::warn("plugins: cannot save {}: {}", key, saved.error().toString());
    }
}

PluginEntry PluginManager::read(const std::filesystem::path& folder, bool dev)
{
    PluginEntry entry;
    entry.folder = folder;
    entry.dev = dev;
    entry.id = text(folder.filename());
    auto manifestText = readText(folder / L"plugin.toml", limits::kManifestMaxBytes);
    if (!manifestText)
    {
        entry.problem = manifestText.error().message();
        return entry;
    }
    domain::Diagnostics diagnostics;
    auto manifest = domain::parsePluginManifest(*manifestText, diagnostics, {});
    if (!manifest)
    {
        entry.problem = manifest.error().message();
        return entry;
    }
    entry.id = manifest->id;
    entry.manifest = std::move(*manifest);
    const Json approved = metaJson(kApprovedMeta, Json::object());
    if (const auto found = approved.find(entry.id); found != approved.end())
    {
        if (auto permissions = plugins::permissionsFromJson(*found))
        {
            entry.approved = std::move(*permissions);
        }
    }
    entry.enabled = contains(metaJson(kEnabledMeta, Json::array()), entry.id);
    entry.disabled = json::stringOr(metaJson(kDisabledMeta, Json::object()), entry.id, "");
    return entry;
}

std::vector<PluginEntry> PluginManager::listLocked()
{
    std::vector<PluginEntry> entries;
    for (const auto& folder : metaJson(kDevMeta, Json::array()))
    {
        if (!folder.is_string())
        {
            continue;
        }
        auto wide = utf8To16(folder.get<std::string>());
        if (!wide)
        {
            continue;
        }
        auto entry = read(std::filesystem::path(std::move(*wide)), true);
        if (std::ranges::none_of(entries, [&](const PluginEntry& other) { return other.id == entry.id; }))
        {
            entries.push_back(std::move(entry));
        }
    }
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(installedFolder(), ec))
    {
        const std::string name = text(item.path().filename());
        if (!item.is_directory(ec) || !domain::isValidPackageId(name))
        {
            continue; // our own folders (".old", ".new") and anything else
        }
        auto entry = read(item.path(), false);
        // A plugin being written wins over the installed copy of the same id.
        if (std::ranges::none_of(entries, [&](const PluginEntry& other) { return other.id == entry.id; }))
        {
            entries.push_back(std::move(entry));
        }
    }
    std::ranges::sort(entries, {}, &PluginEntry::id);
    return entries;
}

std::vector<PluginEntry> PluginManager::list()
{
    const std::scoped_lock lock(m_mutex);
    return listLocked();
}

std::optional<PluginEntry> PluginManager::find(std::string_view id)
{
    const std::scoped_lock lock(m_mutex);
    for (auto& entry : listLocked())
    {
        if (entry.id == id)
        {
            return std::move(entry);
        }
    }
    return std::nullopt;
}

Result<PluginEntry> PluginManager::approve(std::string_view id)
{
    const std::scoped_lock lock(m_mutex);
    auto entry = find(id);
    if (!entry)
    {
        return fail(ErrorCode::NotFound, std::format("plugin '{}' is not installed", id));
    }
    if (!entry->manifest)
    {
        return fail(ErrorCode::InvalidData, std::format("{}: {}", id, entry->problem));
    }
    Json approved = metaJson(kApprovedMeta, Json::object());
    approved[std::string(id)] = plugins::permissionsToJson(entry->manifest->permissions);
    setMetaJson(kApprovedMeta, approved);
    log::info("plugins: {} approved", id);
    return *find(id);
}

Result<PluginEntry> PluginManager::setEnabled(std::string_view id, bool enabled)
{
    const std::scoped_lock lock(m_mutex);
    if (!find(id))
    {
        return fail(ErrorCode::NotFound, std::format("plugin '{}' is not installed", id));
    }
    Json list =
        without(metaJson(kEnabledMeta, Json::array()), [&](const Json& item) { return isText(item, id); });
    if (enabled)
    {
        list.push_back(std::string(id));
    }
    setMetaJson(kEnabledMeta, list);
    Json disabled = metaJson(kDisabledMeta, Json::object());
    disabled.erase(std::string(id));
    setMetaJson(kDisabledMeta, disabled);
    log::info("plugins: {} {}", id, enabled ? "on" : "off");
    return *find(id);
}

void PluginManager::disable(std::string_view id, std::string reason)
{
    const std::scoped_lock lock(m_mutex);
    Json disabled = metaJson(kDisabledMeta, Json::object());
    disabled[std::string(id)] = reason;
    setMetaJson(kDisabledMeta, disabled);
    log::warn("plugins: {} stopped: {}", id, reason);
}

Result<PluginEntry> PluginManager::addDev(const std::filesystem::path& folder)
{
    const std::scoped_lock lock(m_mutex);
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(folder, ec);
    if (ec || !std::filesystem::is_directory(absolute, ec))
    {
        return fail(ErrorCode::NotFound, std::format("'{}' is not a folder", text(folder)));
    }
    auto entry = read(absolute, true);
    if (!entry.manifest)
    {
        return fail(ErrorCode::InvalidData, std::format("plugin.toml: {}", entry.problem));
    }
    Json list = without(metaJson(kDevMeta, Json::array()), [&](const Json& item) {
        if (!item.is_string())
        {
            return true;
        }
        auto wide = utf8To16(item.get<std::string>());
        return !wide || read(std::filesystem::path(*wide), true).id == entry.id; // one folder per id
    });
    if (list.size() >= kMaxDevFolders)
    {
        return fail(ErrorCode::LimitExceeded, "too many plugins in development");
    }
    list.push_back(text(absolute));
    setMetaJson(kDevMeta, list);
    log::info("plugins: {} runs from {} (development)", entry.id, text(absolute));
    return *find(entry.id);
}

Result<void> PluginManager::removeDev(std::string_view id)
{
    const std::scoped_lock lock(m_mutex);
    const Json stored = metaJson(kDevMeta, Json::array());
    const Json list = without(stored, [&](const Json& item) {
        if (!item.is_string())
        {
            return true;
        }
        auto wide = utf8To16(item.get<std::string>());
        return !wide || read(std::filesystem::path(*wide), true).id == id;
    });
    if (list.size() == stored.size())
    {
        return fail(ErrorCode::NotFound, std::format("'{}' is not in development", id));
    }
    setMetaJson(kDevMeta, list);
    return {};
}

Result<PluginEntry> PluginManager::install(const std::filesystem::path& source)
{
    const std::scoped_lock lock(m_mutex);
    auto incoming = read(source, false);
    if (!incoming.manifest)
    {
        return fail(ErrorCode::InvalidData, std::format("plugin.toml: {}", incoming.problem));
    }
    const std::string id = incoming.id;
    const auto target = installedFolder() / std::filesystem::path(utf8To16Lossy(id));
    const auto staging = installedFolder() / std::filesystem::path(utf8To16Lossy(id + ".new"));
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot create {}: {}", text(staging), ec.message()));
    }
    // Every file: a safe relative name, a regular file (no links), within the limits.
    std::size_t files = 0;
    std::uint64_t total = 0;
    auto root = platform::finalPath(source);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    for (auto it = std::filesystem::recursive_directory_iterator(*root, ec); !ec && it != decltype(it){};
         it.increment(ec))
    {
        if (isReparsePoint(it->path()))
        {
            std::filesystem::remove_all(staging, ec);
            return fail(ErrorCode::AccessDenied,
                        std::format("{}: links and junctions are not allowed in a plugin", text(it->path())));
        }
        const auto relative = std::filesystem::relative(it->path(), *root, ec);
        auto valid = domain::validatePackagePath(utf16To8Lossy(relative.generic_wstring()));
        if (!valid)
        {
            std::filesystem::remove_all(staging, ec);
            return fail(ErrorCode::InvalidData,
                        std::format("{}: {}", text(relative), valid.error().message()));
        }
        const auto destination = staging / relative;
        if (it->is_directory(ec))
        {
            std::filesystem::create_directories(destination, ec);
            continue;
        }
        if (++files > kInstallMaxFiles)
        {
            std::filesystem::remove_all(staging, ec);
            return fail(ErrorCode::LimitExceeded, std::format("more than {} files", kInstallMaxFiles));
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        auto copied = platform::copyHashed(it->path(), destination, kInstallMaxBytes - total);
        if (!copied)
        {
            std::filesystem::remove_all(staging, ec);
            return std::unexpected(std::move(copied.error()).withContext(text(relative)));
        }
        total += copied->bytes;
    }
    if (ec)
    {
        std::filesystem::remove_all(staging, ec);
        return fail(ErrorCode::Io, std::format("reading {}: {}", text(source), ec.message()));
    }
    const auto old = installedFolder() / std::filesystem::path(utf8To16Lossy(id + ".old"));
    std::filesystem::remove_all(old, ec);
    if (std::filesystem::exists(target, ec))
    {
        std::filesystem::rename(target, old, ec);
    }
    std::filesystem::rename(staging, target, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot put {} in place: {}", id, ec.message()));
    }
    std::filesystem::remove_all(old, ec);
    log::info("plugins: {} installed ({} files)", id, files);
    return *find(id);
}

Result<void> PluginManager::remove(std::string_view id)
{
    const std::scoped_lock lock(m_mutex);
    if (!domain::isValidPackageId(id))
    {
        return fail(ErrorCode::InvalidArgument, "invalid plugin id");
    }
    const auto target = installedFolder() / std::filesystem::path(utf8To16Lossy(id));
    std::error_code ec;
    if (!std::filesystem::is_directory(target, ec))
    {
        return fail(ErrorCode::NotFound, std::format("plugin '{}' is not installed", id));
    }
    std::filesystem::remove_all(target, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot remove {}: {}", id, ec.message()));
    }
    for (const auto key : {kApprovedMeta, kDisabledMeta})
    {
        Json object = metaJson(key, Json::object());
        object.erase(std::string(id));
        setMetaJson(key, object);
    }
    setMetaJson(kEnabledMeta, without(metaJson(kEnabledMeta, Json::array()),
                                      [&](const Json& item) { return isText(item, id); }));
    std::filesystem::remove(m_data / L"plugin-data" / (utf8To16Lossy(id) + L".json"), ec);
    log::info("plugins: {} removed", id);
    return {};
}

void PluginManager::setRicePlugins(RicePlugins rice)
{
    const std::scoped_lock lock(m_mutex);
    Json settings = Json::object();
    for (const auto& [id, value] : rice.settings)
    {
        settings[id] = json::fromValue(value);
    }
    setMetaJson(kRiceMeta, Json{{"required", rice.required}, {"settings", std::move(settings)}});
}

RicePlugins PluginManager::ricePlugins()
{
    const std::scoped_lock lock(m_mutex);
    const Json stored = metaJson(kRiceMeta, Json::object());
    RicePlugins rice;
    for (const auto& id : json::memberOr(stored, "required", Json::array()))
    {
        if (id.is_string() && domain::isValidPackageId(id.get<std::string>()))
        {
            rice.required.push_back(id.get<std::string>());
        }
    }
    const Json settings = json::memberOr(stored, "settings", Json::object());
    if (settings.is_object())
    {
        for (const auto& [id, value] : settings.items())
        {
            if (auto converted = json::toValue(value))
            {
                rice.settings.emplace_back(id, std::move(*converted));
            }
        }
    }
    return rice;
}

domain::Value PluginManager::settingsFor(const domain::PluginManifest& manifest, const RicePlugins& rice,
                                         const domain::Value* item, std::string_view where)
{
    domain::Diagnostics diagnostics;
    const std::vector<const domain::Value*> layers{rice.settingsOf(manifest.id), item};
    domain::Value resolved = domain::resolvePluginConfig(manifest.config, layers, diagnostics, where);
    for (const auto& diagnostic : diagnostics.all())
    {
        log::warn("plugins: {}: {}", diagnostic.key, diagnostic.message);
    }
    return resolved;
}

Result<bar::Plan> PluginManager::barPlan(const domain::Rice& rice)
{
    const std::scoped_lock lock(m_mutex);
    const auto entries = listLocked();
    const auto entryOf = [&](std::string_view id) -> const PluginEntry* {
        const auto found = std::ranges::find(entries, id, &PluginEntry::id);
        return found == entries.end() || !found->manifest ? nullptr : &*found;
    };
    domain::Rice skinned = rice;
    for (auto& config : skinned.bars)
    {
        if (config.skin.empty())
        {
            continue;
        }
        const PluginEntry* skin = entryOf(config.skin);
        if (skin == nullptr || !skin->manifest->skin)
        {
            log::warn("bar: skin '{}' is not installed; the bar uses its own settings", config.skin);
            continue;
        }
        config = domain::applySkin(config, *skin->manifest->skin);
    }
    auto plan = bar::planFromRice(skinned);
    if (!plan)
    {
        return plan;
    }
    RicePlugins settings;
    for (const auto& entry : rice.pluginSettings)
    {
        settings.settings.emplace_back(entry.id, entry.config);
    }
    for (auto& config : plan->bars)
    {
        for (auto* zone : {&config.left, &config.center, &config.right})
        {
            for (auto& item : *zone)
            {
                if (!item.isPlugin())
                {
                    continue;
                }
                const PluginEntry* entry = entryOf(item.id);
                if (entry == nullptr || !entry->manifest->widget)
                {
                    log::warn("bar: plugin widget '{}' is not installed", item.id);
                    continue;
                }
                item.config =
                    settingsFor(*entry->manifest, settings, &item.config, std::format("bar.{}", item.id));
                if (std::ranges::none_of(plan->plugins,
                                         [&](const bar::PlanPlugin& p) { return p.id == item.id; }))
                {
                    plan->plugins.push_back({entry->id, entry->folder, entry->dev});
                }
            }
        }
    }
    return plan;
}

plugins::HostPlan PluginManager::hostPlan(const std::optional<bar::Plan>& bars)
{
    const std::scoped_lock lock(m_mutex);
    const RicePlugins rice = ricePlugins();
    plugins::HostPlan plan;
    for (const auto& entry : listLocked())
    {
        const bool onBar = bars && std::ranges::any_of(bars->plugins, [&](const bar::PlanPlugin& p) {
                               return p.id == entry.id;
                           });
        const bool wanted =
            entry.enabled || onBar || std::ranges::find(rice.required, entry.id) != rice.required.end();
        if (!wanted || !entry.manifest || !entry.manifest->hasScript())
        {
            continue;
        }
        if (!entry.disabled.empty())
        {
            log::info("plugins: {} stays off: {} (rmw plugin enable {})", entry.id, entry.disabled, entry.id);
            continue;
        }
        if (entry.needsApproval())
        {
            log::warn("plugins: {} waits for approval of its permissions (rmw plugin approve {})", entry.id,
                      entry.id);
            continue;
        }
        plugins::HostPlugin plugin;
        plugin.id = entry.id;
        plugin.folder = entry.folder;
        plugin.dev = entry.dev;
        plugin.granted =
            entry.manifest->permissions.intersect(entry.approved.value_or(domain::PluginPermissions{}));
        plugin.config = settingsFor(*entry.manifest, rice, nullptr, std::format("plugins.{}", entry.id));
        plan.plugins.push_back(std::move(plugin));
    }
    return plan;
}

} // namespace rmw::core
