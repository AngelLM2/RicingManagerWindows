#pragma once

#include "rmw/base/Error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// File operations with the guarantees docs/03 §7 and docs/06 §4 ask for: SHA-256 via CNG,
// atomic replacement, and copies that refuse reparse points and report where the source
// really lives.
namespace rmw::platform
{

using Sha256Digest = std::array<std::uint8_t, 32>;

// Incremental SHA-256 (BCrypt). Move-only.
class Sha256
{
public:
    [[nodiscard]] static Result<Sha256> create();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&& other) noexcept;
    Sha256& operator=(Sha256&& other) noexcept;

    [[nodiscard]] Result<void> update(std::span<const std::byte> bytes);
    [[nodiscard]] Result<Sha256Digest> finish(); // the object cannot be updated afterwards

private:
    explicit Sha256(void* handle) noexcept;
    void* m_hash = nullptr; // BCRYPT_HASH_HANDLE
};

[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes); // lower-case
// `byteCount` bytes from the system CSPRNG, as hex (temporary names, pipe names).
[[nodiscard]] Result<std::string> randomHex(std::size_t byteCount);

struct HashedCopy
{
    std::string sha256Hex;
    std::uint64_t bytes = 0;
    std::filesystem::path sourceFinalPath; // the source after resolving every link (\\?\ removed)
};

// Copies `source` to `target` (which must not exist), hashing on the way and flushing the
// target to disk. The source must be a regular file — symbolic links, junctions and other
// reparse points are refused — and at most `maxBytes` long. On failure `target` is removed.
[[nodiscard]] Result<HashedCopy> copyHashed(const std::filesystem::path& source,
                                            const std::filesystem::path& target, std::uint64_t maxBytes);

// Reads a whole regular file (no links or reparse points) of at most `maxBytes`.
[[nodiscard]] Result<std::vector<std::byte>> readFileLimited(const std::filesystem::path& path,
                                                             std::uint64_t maxBytes);

// The final path of an open file (a HANDLE), every link resolved and \\?\ removed.
[[nodiscard]] Result<std::filesystem::path> finalPathOfHandle(void* handle);
// Writes every byte to an open file (a HANDLE).
[[nodiscard]] Result<void> writeAll(void* file, std::span<const std::byte> bytes);

// Renames `from` over `to` atomically (POSIX semantics where the volume supports them).
[[nodiscard]] Result<void> replaceFile(const std::filesystem::path& from, const std::filesystem::path& to);

// Writes `bytes` to `path` via a flushed temporary file and replaceFile(): readers see the old
// or the new content, never a mix.
[[nodiscard]] Result<void> writeFileAtomic(const std::filesystem::path& path,
                                           std::span<const std::byte> bytes);

// %VARS% expanded (UTF-8 in, path out).
[[nodiscard]] Result<std::filesystem::path> expandEnvironment(std::string_view text);

// The type of a file by its first bytes: "jpg", "png", "bmp", "gif", "cur", "ani", or empty
// when unknown or unreadable.
[[nodiscard]] std::string sniffFileType(const std::filesystem::path& file);

// The path with every link resolved (GetFinalPathNameByHandle), without the \\?\ prefix.
[[nodiscard]] Result<std::filesystem::path> finalPath(const std::filesystem::path& path);

// True when `child` is `root` or lies under it (ordinal, case-insensitive, both already final).
[[nodiscard]] bool isWithin(const std::filesystem::path& child, const std::filesystem::path& root);

} // namespace rmw::platform
