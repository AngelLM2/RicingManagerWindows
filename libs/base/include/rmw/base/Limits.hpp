#pragma once

#include <cstddef>
#include <cstdint>

// Every limit applied to external input lives here (docs/06 §4, docs/12 §6, docs/08 §3 rule 8).
namespace rmw::limits
{

// Packages (.rice / .rmwp)
inline constexpr std::size_t kPackageMaxEntries = 1'000;
inline constexpr std::uint64_t kPackageMaxTotalBytes = 1ull << 30; // 1 GiB, counted as decompressed
inline constexpr std::uint32_t kPackageMaxCompressionRatio = 100;
inline constexpr std::uint64_t kVideoMaxBytes = 512ull << 20;
inline constexpr std::uint64_t kImageMaxBytes = 64ull << 20;
inline constexpr std::uint64_t kCursorMaxBytes = 1ull << 20;
inline constexpr std::uint64_t kInfMaxBytes = 64ull << 10;
inline constexpr std::uint64_t kManifestMaxBytes = 64ull << 10; // rice.toml, plugin.toml
inline constexpr std::size_t kPathComponentMaxChars = 255;
inline constexpr std::size_t kRelativePathMaxChars = 160;

// Images
inline constexpr std::uint32_t kImageMaxSide = 16'384;
inline constexpr std::uint64_t kImageMaxPixels = 64ull * 1000 * 1000;
inline constexpr std::uint32_t kAnimatedImageMaxSide = 4'096;

// rice.toml / plugin.toml contents
inline constexpr std::size_t kShortTextMaxBytes = 128;  // names, authors, licenses, ids
inline constexpr std::size_t kLongTextMaxBytes = 2'048; // descriptions
inline constexpr std::size_t kRiceMaxMonitorRules = 16;
inline constexpr std::size_t kRiceMaxBars = 16;
inline constexpr std::size_t kBarMaxItemsPerZone = 32;
inline constexpr std::size_t kPaletteMaxColors = 64;
inline constexpr std::size_t kRiceMaxPluginRequirements = 64;
inline constexpr std::size_t kConfigValueMaxElements = 1'000; // per plugin/widget config tree

// Structured data
inline constexpr std::size_t kTomlMaxDepth = 32;
inline constexpr std::size_t kJsonMaxDepth = 32;
inline constexpr std::size_t kJsonMaxElements = 100'000;

// IPC
inline constexpr std::size_t kIpcMaxMessageBytes = 1u << 20;

// Plugins
inline constexpr std::size_t kPluginVmMaxBytes = 16u << 20;
inline constexpr std::uint32_t kPluginCallbackMaxMs = 50;
inline constexpr std::uint32_t kPluginMinUpdateIntervalMs = 250;
inline constexpr std::size_t kPluginStorageMaxBytes = 1u << 20;
inline constexpr std::size_t kPluginNetResponseMaxBytes = 1u << 20;

// Logging
inline constexpr std::uint64_t kLogFileMaxBytes = 2ull << 20;
inline constexpr std::uint32_t kLogMaxFiles = 5;
inline constexpr std::size_t kLogMessageMaxBytes = 16u << 10;

} // namespace rmw::limits
