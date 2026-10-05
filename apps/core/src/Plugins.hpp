#pragma once

#include "rmw/bar/Plan.hpp"
#include "rmw/base/Error.hpp"
#include "rmw/domain/PluginManifest.hpp"
#include "rmw/engine/StateDb.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/plugins/HostPlan.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The core's side of plugins (docs/12): which are installed (<data>\plugins\<id>) or being written
// (rmw plugin dev), what the user approved, which are on, and what the bar and the plugin host
// get from that. The core never runs plugin code: it reads manifests and copies files.
namespace rmw::core
{

// What the active rice says about plugins ([plugins] requires and [plugins."id"]).
struct RicePlugins
{
    std::vector<std::string> required;
    std::vector<std::pair<std::string, domain::Value>> settings;
    // The rice's palette ("base" -> "#1a1b26") and accent: plugin settings may name them.
    std::vector<std::pair<std::string, std::string>> palette;
    std::string accent; // "#RRGGBB"; empty: Windows' own

    [[nodiscard]] static RicePlugins of(const domain::Rice& rice);

    [[nodiscard]] const domain::Value* settingsOf(std::string_view id) const noexcept;
};

struct PluginEntry
{
    std::string id;
    std::filesystem::path folder;
    bool dev = false;
    std::optional<domain::PluginManifest> manifest;
    std::string problem; // why plugin.toml did not load
    std::optional<domain::PluginPermissions> approved;
    bool enabled = false; // switched on by the user (actions and rules run without a rice)
    std::string disabled; // stopped after repeated failures: why

    // It asks for more than the user approved (nothing asked needs nothing).
    [[nodiscard]] bool needsApproval() const;
    [[nodiscard]] json::Json toJson() const;
};

class PluginManager
{
public:
    PluginManager(std::filesystem::path dataDirectory, engine::StateDb& state);

    [[nodiscard]] std::filesystem::path installedFolder() const { return m_data / L"plugins"; }

    [[nodiscard]] std::vector<PluginEntry> list();
    [[nodiscard]] std::optional<PluginEntry> find(std::string_view id);

    // The user saw the permissions and said yes: they are approved as the manifest asks now.
    [[nodiscard]] Result<PluginEntry> approve(std::string_view id);
    // On or off for good (also clears a stop after failures).
    [[nodiscard]] Result<PluginEntry> setEnabled(std::string_view id, bool enabled);
    // Stops a plugin that failed too often until the user turns it on again.
    void disable(std::string_view id, std::string reason);
    // A folder being written: it runs from there and reloads when saved.
    [[nodiscard]] Result<PluginEntry> addDev(const std::filesystem::path& folder);
    [[nodiscard]] Result<void> removeDev(std::string_view id);
    // Copies a plugin folder into <data>\plugins (replacing the same id). Every file is checked
    // (relative names, no links, size limits); what it asks must be approved before it runs.
    [[nodiscard]] Result<PluginEntry> install(const std::filesystem::path& source);
    [[nodiscard]] Result<void> remove(std::string_view id);

    void setRicePlugins(RicePlugins rice);
    [[nodiscard]] RicePlugins ricePlugins();

    // The rice's bars with their skins applied, plugin widgets' settings resolved and the folders
    // of the plugin widgets they show.
    [[nodiscard]] Result<bar::Plan> barPlan(const domain::Rice& rice);
    // The values of the tweak plugins the rice requires (the "tweaks" module applies and undoes
    // them). `notes` gets what the user should know: a plugin missing, Explorer to restart.
    [[nodiscard]] std::vector<domain::Tweak> tweaksFor(const domain::Rice& rice,
                                                       std::vector<std::string>& notes);
    // The script plugins to run now: switched on, being written, required by the rice, or on a
    // bar — installed, approved and not stopped.
    [[nodiscard]] plugins::HostPlan hostPlan(const std::optional<bar::Plan>& bars);

private:
    [[nodiscard]] PluginEntry read(const std::filesystem::path& folder, bool dev);
    [[nodiscard]] std::vector<PluginEntry> listLocked();
    [[nodiscard]] json::Json metaJson(std::string_view key, json::Json fallback);
    void setMetaJson(std::string_view key, const json::Json& value);
    [[nodiscard]] domain::Value settingsFor(const domain::PluginManifest& manifest, const RicePlugins& rice,
                                            const domain::Value* item, std::string_view where);

    std::filesystem::path m_data;
    engine::StateDb& m_state;
    std::recursive_mutex m_mutex;
};

} // namespace rmw::core
