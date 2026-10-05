#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/PluginManifest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Reading a plugin's files, with the same checks wherever it happens (the bar, the CLI's
// validator, the core's catalog): limits, no links out of the folder.
namespace rmw::plugins
{

inline constexpr std::uint64_t kPluginImageMaxBytes = 2u << 20;
inline constexpr std::uint32_t kPluginImageMaxSide = 1'024;

// A text file of at most `maxBytes` (a regular file: no links or reparse points).
[[nodiscard]] Result<std::string> readPluginText(const std::filesystem::path& file, std::uint64_t maxBytes);

struct LoadedManifest
{
    domain::PluginManifest manifest;
    domain::Diagnostics diagnostics; // warnings (errors make the call fail)
};

// <folder>\plugin.toml, read and checked.
[[nodiscard]] Result<LoadedManifest> readPluginManifest(const std::filesystem::path& folder,
                                                        domain::Diagnostics* diagnostics = nullptr);

// A picture of the plugin for the bar: inside its folder once every link is resolved, a regular
// file, a .png/.jpg/.svg of at most kPluginImageMaxBytes and (PNG, JPEG) at most
// kPluginImageMaxSide pixels a side (the header says; a small file can claim a huge image).
[[nodiscard]] Result<std::filesystem::path> pluginImageFile(const std::filesystem::path& folder,
                                                            std::string_view packagePath);

} // namespace rmw::plugins
