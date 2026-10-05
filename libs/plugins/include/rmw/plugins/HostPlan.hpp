#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/PluginManifest.hpp"
#include "rmw/domain/Value.hpp"
#include "rmw/json/Json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What the core hands rmw-plugin-host: the script plugins to run, each with what the user approved
// and its resolved settings, written to <data>\plugin-host\plan.json (like the bar's plan). The
// host reads each plugin's own files again and checks them like any input.
namespace rmw::plugins
{

inline constexpr int kHostPlanVersion = 1;

struct HostPlugin
{
    std::string id;
    std::filesystem::path folder;
    domain::PluginPermissions granted;
    domain::Value config;
    // A plugin being written (rmw plugin dev): reloaded whenever its files change.
    bool dev = false;

    friend bool operator==(const HostPlugin&, const HostPlugin&) = default;
};

struct HostPlan
{
    std::vector<HostPlugin> plugins;
};

// What the plugin host keeps in a mapping the core gave it: the plugin whose callback runs now. When
// the host is ended (hung, out of memory, crashed), the core reads it to know whom to blame and stops
// that plugin after it brought the host down twice (docs/12 §6).
struct BlameRecord
{
    std::uint32_t version = 1;
    char plugin[128] = {};
};

[[nodiscard]] std::string toJson(const HostPlan& plan);
[[nodiscard]] Result<HostPlan> hostPlanFromJson(std::string_view text);

[[nodiscard]] json::Json permissionsToJson(const domain::PluginPermissions& permissions);
[[nodiscard]] Result<domain::PluginPermissions> permissionsFromJson(const json::Json& json);

} // namespace rmw::plugins
