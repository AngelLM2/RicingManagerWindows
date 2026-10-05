#include "rmw/plugins/HostPlan.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/json/Value.hpp"

#include <algorithm>
#include <format>

namespace rmw::plugins
{
namespace
{

using json::Json;

constexpr std::size_t kMaxPlugins = 256;
constexpr std::size_t kMaxNames = 64;

Result<std::vector<std::string>> names(const Json& object, std::string_view key,
                                       bool (*valid)(std::string_view))
{
    std::vector<std::string> list;
    const Json items = json::memberOr(object, key, Json::array());
    if (!items.is_array() || items.size() > kMaxNames)
    {
        return fail(ErrorCode::InvalidData, std::format("permissions.{}: expected a short list", key));
    }
    for (const auto& item : items)
    {
        if (!item.is_string() || !valid(item.get<std::string>()))
        {
            return fail(ErrorCode::InvalidData, std::format("permissions.{}: invalid entry", key));
        }
        list.push_back(item.get<std::string>());
    }
    return list;
}

bool knownProvider(std::string_view name)
{
    return domain::findPluginProvider(name) != nullptr;
}

bool knownAction(std::string_view name)
{
    return domain::findPluginAction(name) != nullptr;
}

std::string pathText(const std::filesystem::path& path)
{
    return utf16To8Lossy(path.native());
}

} // namespace

Json permissionsToJson(const domain::PluginPermissions& permissions)
{
    return Json{{"providers", permissions.providers},
                {"actions", permissions.actions},
                {"network", permissions.network},
                {"storage", permissions.storage}};
}

Result<domain::PluginPermissions> permissionsFromJson(const Json& object)
{
    if (!object.is_object())
    {
        return fail(ErrorCode::InvalidData, "permissions: expected an object");
    }
    domain::PluginPermissions permissions;
    auto providers = names(object, "providers", knownProvider);
    auto actions = names(object, "actions", knownAction);
    auto network = names(object, "network", domain::isValidNetworkDomain);
    if (!providers || !actions || !network)
    {
        return std::unexpected(!providers ? providers.error() : !actions ? actions.error() : network.error());
    }
    permissions.providers = std::move(*providers);
    permissions.actions = std::move(*actions);
    permissions.network = std::move(*network);
    permissions.storage = json::boolOr(object, "storage", false);
    return permissions;
}

std::string toJson(const HostPlan& plan)
{
    Json plugins = Json::array();
    for (const auto& plugin : plan.plugins)
    {
        plugins.push_back({{"id", plugin.id},
                           {"folder", pathText(plugin.folder)},
                           {"granted", permissionsToJson(plugin.granted)},
                           {"config", json::fromValue(plugin.config)},
                           {"dev", plugin.dev}});
    }
    return json::dumpPretty(Json{{"version", kHostPlanVersion}, {"plugins", std::move(plugins)}});
}

Result<HostPlan> hostPlanFromJson(std::string_view text)
{
    auto parsed = json::parse(text);
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    if (!root.is_object() || json::intOr(root, "version", 0) != kHostPlanVersion)
    {
        return fail(ErrorCode::InvalidData,
                    std::format("not a plugin host plan of version {}", kHostPlanVersion));
    }
    const Json plugins = json::memberOr(root, "plugins", Json::array());
    if (!plugins.is_array() || plugins.size() > kMaxPlugins)
    {
        return fail(ErrorCode::InvalidData, "plugins: expected a list");
    }
    HostPlan plan;
    for (const auto& entry : plugins)
    {
        if (!entry.is_object())
        {
            return fail(ErrorCode::InvalidData, "plugins: expected objects");
        }
        HostPlugin plugin;
        plugin.id = json::stringOr(entry, "id", "");
        if (!domain::isValidPackageId(plugin.id))
        {
            return fail(ErrorCode::InvalidData, std::format("invalid plugin id '{}'", plugin.id));
        }
        auto folder = utf8To16(json::stringOr(entry, "folder", ""));
        if (!folder || folder->empty())
        {
            return fail(ErrorCode::InvalidData, std::format("{}: invalid folder", plugin.id));
        }
        plugin.folder = std::filesystem::path(std::move(*folder));
        if (!plugin.folder.is_absolute())
        {
            return fail(ErrorCode::InvalidData, std::format("{}: the folder must be absolute", plugin.id));
        }
        auto granted = permissionsFromJson(json::memberOr(entry, "granted", Json::object()));
        if (!granted)
        {
            return std::unexpected(std::move(granted.error()).withContext(plugin.id));
        }
        plugin.granted = std::move(*granted);
        auto config = json::toValue(json::memberOr(entry, "config", Json::object()));
        if (!config)
        {
            return std::unexpected(std::move(config.error()).withContext(plugin.id));
        }
        plugin.config = std::move(*config);
        plugin.dev = json::boolOr(entry, "dev", false);
        if (std::ranges::any_of(plan.plugins, [&](const HostPlugin& other) { return other.id == plugin.id; }))
        {
            return fail(ErrorCode::InvalidData, std::format("{} is listed twice", plugin.id));
        }
        plan.plugins.push_back(std::move(plugin));
    }
    return plan;
}

} // namespace rmw::plugins
