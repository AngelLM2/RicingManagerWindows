#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/engine/Module.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace rmw::engine
{

// Content-addressed copies of every file a rice or snapshot uses: store\<sha256>.<ext>
// (docs/03 §5.3). The Windows settings point here, so deleting a rice never breaks the active
// look, and the original look can always come back.
class ContentStore
{
public:
    explicit ContentStore(std::filesystem::path directory);

    // Copies a regular file in (deduplicated by content) and returns its stored path.
    // `extension` (without dot) overrides the source's, e.g. for extension-less system files.
    [[nodiscard]] Result<std::filesystem::path> import(const std::filesystem::path& source,
                                                       std::uint64_t maxBytes,
                                                       std::string_view extension = {});

    // Deletes stored files whose hash `isReferenced` does not recognize; returns how many.
    [[nodiscard]] Result<std::size_t> collectGarbage(
        const std::function<bool(std::string_view sha256)>& isReferenced);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }

private:
    std::filesystem::path m_directory;
};

// IAssetSink for one operation: package files come from `packageRoot` (and must really be
// inside it once every link is resolved); system files from anywhere the module captured.
class StoreAssetSink final : public IAssetSink
{
public:
    StoreAssetSink(ContentStore& store, std::filesystem::path packageRoot);

    [[nodiscard]] Result<std::filesystem::path> fromPackage(std::string_view relativePath) override;
    [[nodiscard]] Result<std::vector<std::byte>> readPackageFile(std::string_view relativePath,
                                                                 std::uint64_t maxBytes) override;
    [[nodiscard]] Result<std::filesystem::path> fromSystem(const std::filesystem::path& file,
                                                           std::string_view extension) override;

private:
    // The file's final path, refusing anything that resolves outside the package.
    [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view relativePath) const;

    ContentStore& m_store;
    std::filesystem::path m_packageRoot;
};

// Size limit for an asset by extension (docs/06 §4).
[[nodiscard]] std::uint64_t assetSizeLimit(std::string_view extension) noexcept;

} // namespace rmw::engine
