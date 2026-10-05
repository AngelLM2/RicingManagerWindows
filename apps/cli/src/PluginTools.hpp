#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/PluginManifest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// `rmw plugin new | validate | pack` (docs/12 §8): they work on a folder and never need the core.
namespace rmw::cli
{

struct PluginCheck
{
    std::optional<domain::PluginManifest> manifest;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// The same checks as installing and running: plugin.toml, the script compiles, the widget's .slint
// passes the bar's checker and matches [widget], its pictures are the plugin's own and small.
[[nodiscard]] PluginCheck checkPlugin(const std::filesystem::path& folder);

// The permissions as the install prompt shows them; "!" marks the sensitive ones.
[[nodiscard]] std::vector<std::string> describePermissions(const domain::PluginPermissions& permissions);

// A new plugin from a template: widget, action, rule, tweak or skin. The folder must not exist.
[[nodiscard]] Result<void> newPlugin(std::string_view kind, const std::filesystem::path& folder,
                                     std::string_view id);

// <folder> as a .rmwp (a zip): every file but hidden ones, names checked, no links.
[[nodiscard]] Result<std::uint64_t> packPlugin(const std::filesystem::path& folder,
                                               const std::filesystem::path& out);

} // namespace rmw::cli
