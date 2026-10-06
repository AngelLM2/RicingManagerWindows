#pragma once

#include "rmw/base/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Packages from outside (docs/06 §4): a .rice (a rice to share) or a .rmwp (a plugin), both zip.
// Read with libzip entry by entry, never "extract all": every name, size, type and content is
// checked before a byte reaches the disk, and only into an empty staging folder the caller made.
// The core runs this in rmw-core --import-worker, a process of its own in a Job with a memory cap.
namespace rmw::package
{

enum class Kind : std::uint8_t
{
    Rice,   // rice.toml, images, videos, cursors (+ install.inf), credits and licenses
    Plugin, // plugin.toml, .luau, .slint, images (+ .svg), data (.json), credits and licenses
};

// A name inside the zip (from its central directory), checked: the rules of docs/06 §4 on top of
// domain::validatePackagePath. The normalized relative path, or why not.
[[nodiscard]] Result<std::string> checkEntryName(std::string_view name);
// Whether a package of `kind` may hold a file with this extension.
[[nodiscard]] bool allowedFile(Kind kind, std::string_view path);
// The most a file of this type may weigh.
[[nodiscard]] std::uint64_t maxFileBytes(std::string_view path);
// What the file's own bytes say it is matches its extension, within limits (images: their
// declared size; cursors: their structure; videos: an MP4 box). `head` is the whole file, except
// for videos (their first bytes).
[[nodiscard]] Result<void> checkContent(std::string_view path, std::span<const std::byte> head);

struct Imported
{
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::string stripped; // the one folder every entry was in, left out ("" when none)
};

// Every entry of `archive` checked and written under `staging` (empty, made by the caller on the
// same volume as the final place). A single top folder holding the manifest is left out. Nothing
// is written when `staging` is empty (a check only, e.g. for fuzzing).
[[nodiscard]] Result<Imported> extract(const std::filesystem::path& archive, Kind kind,
                                       const std::filesystem::path& staging);
[[nodiscard]] Result<Imported> extract(std::span<const std::byte> archive, Kind kind,
                                       const std::filesystem::path& staging);

// A zip of `folder`'s files (stored, UTF-8 names, the same files make the same bytes) written to
// `out`, for `rmw pack` and `rmw plugin pack`: only the files `kind` allows (editor files and
// dot-folders stay out), no links, within the package limits. The size of the archive.
[[nodiscard]] Result<std::uint64_t> pack(const std::filesystem::path& folder, Kind kind,
                                         const std::filesystem::path& out);

} // namespace rmw::package
