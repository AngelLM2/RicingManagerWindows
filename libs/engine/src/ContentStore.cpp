#include "rmw/engine/ContentStore.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <algorithm>
#include <format>

namespace rmw::engine
{
namespace
{

constexpr std::wstring_view kIncomingPrefix = L".incoming-";
constexpr std::size_t kShaHexLength = 64;

// Lower-case [a-z0-9]{1,10} without the dot, or empty when unusable.
std::string cleanExtension(std::string_view extension)
{
    if (extension.starts_with('.'))
    {
        extension.remove_prefix(1);
    }
    if (extension.empty() || extension.size() > 10)
    {
        return {};
    }
    std::string clean;
    for (const char c : extension)
    {
        if (c >= 'A' && c <= 'Z')
        {
            clean += static_cast<char>(c - 'A' + 'a');
        }
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            clean += c;
        }
        else
        {
            return {};
        }
    }
    return clean;
}

std::string extensionOf(const std::filesystem::path& path)
{
    // Lossy, never throwing: an extension that does not convert is no extension cleanExtension keeps.
    return cleanExtension(utf16To8Lossy(path.extension().native()));
}

// "<64 hex>" or "<64 hex>.<ext>" -> the hash; empty otherwise.
std::string hashOfStoreName(std::wstring_view name)
{
    if (name.size() < kShaHexLength || (name.size() > kShaHexLength && name[kShaHexLength] != L'.'))
    {
        return {};
    }
    std::string hash;
    for (std::size_t i = 0; i < kShaHexLength; ++i)
    {
        const wchar_t c = name[i];
        const bool hexDigit = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        if (!hexDigit)
        {
            return {};
        }
        hash += static_cast<char>(c);
    }
    return hash;
}

} // namespace

std::uint64_t assetSizeLimit(std::string_view extension) noexcept
{
    const auto is = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::find(names, extension) != names.end();
    };
    if (is({"cur", "ani"}))
    {
        return limits::kCursorMaxBytes;
    }
    if (is({"wav"}))
    {
        return limits::kSoundMaxBytes;
    }
    if (is({"ico"}))
    {
        return limits::kIconMaxBytes;
    }
    if (is({"mp4", "m4v", "mov", "webm"}))
    {
        return limits::kVideoMaxBytes;
    }
    if (is({"inf", "theme", "toml"}))
    {
        return limits::kInfMaxBytes;
    }
    return limits::kImageMaxBytes;
}

ContentStore::ContentStore(std::filesystem::path directory)
    : m_directory(std::move(directory))
{
}

Result<std::filesystem::path> ContentStore::import(const std::filesystem::path& source,
                                                   std::uint64_t maxBytes, std::string_view extension)
{
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot create the content store: {}", ec.message()),
                    ec.value());
    }

    const std::string cleanExt = extension.empty() ? extensionOf(source) : cleanExtension(extension);
    auto suffix = platform::randomHex(8);
    if (!suffix)
    {
        return std::unexpected(std::move(suffix.error()));
    }
    std::filesystem::path incoming = m_directory / kIncomingPrefix;
    incoming += std::filesystem::path(*suffix);

    auto copy = platform::copyHashed(source, incoming, maxBytes);
    if (!copy)
    {
        return std::unexpected(std::move(copy.error()).withContext("store file"));
    }

    std::filesystem::path stored = m_directory / std::filesystem::path(copy->sha256Hex);
    if (!cleanExt.empty())
    {
        stored += std::filesystem::path("." + cleanExt);
    }
    if (std::filesystem::exists(stored, ec))
    {
        std::filesystem::remove(incoming, ec); // same content already stored
        return stored;
    }
    if (auto replaced = platform::replaceFile(incoming, stored); !replaced)
    {
        std::filesystem::remove(incoming, ec);
        return std::unexpected(std::move(replaced.error()));
    }
    return stored;
}

Result<std::filesystem::path> ContentStore::importBytes(std::span<const std::byte> bytes,
                                                        std::string_view extension)
{
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot create the content store: {}", ec.message()),
                    ec.value());
    }
    auto digest = platform::sha256(bytes);
    if (!digest)
    {
        return std::unexpected(std::move(digest.error()));
    }
    std::filesystem::path stored = m_directory / std::filesystem::path(platform::toHex(*digest));
    if (const std::string cleanExt = cleanExtension(extension); !cleanExt.empty())
    {
        stored += std::filesystem::path("." + cleanExt);
    }
    if (std::filesystem::exists(stored, ec))
    {
        return stored;
    }
    if (auto written = platform::writeFileAtomic(stored, bytes); !written)
    {
        return std::unexpected(std::move(written.error()));
    }
    return stored;
}

Result<std::size_t> ContentStore::collectGarbage(
    const std::function<bool(std::string_view sha256)>& isReferenced)
{
    std::error_code ec;
    if (!std::filesystem::exists(m_directory, ec))
    {
        return std::size_t{0};
    }
    std::vector<std::filesystem::path> doomed;
    for (std::filesystem::directory_iterator it(m_directory, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::wstring name = it->path().filename().native();
        if (name.starts_with(kIncomingPrefix))
        {
            doomed.push_back(it->path()); // left over from an interrupted import
            continue;
        }
        const std::string hash = hashOfStoreName(name);
        if (!hash.empty() && !isReferenced(hash))
        {
            doomed.push_back(it->path());
        }
    }
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot list the content store: {}", ec.message()),
                    ec.value());
    }

    std::size_t removed = 0;
    for (const auto& path : doomed)
    {
        if (std::filesystem::remove(path, ec))
        {
            ++removed;
        }
        else if (ec)
        {
            // In use (e.g. a cursor Windows still holds open): try again next time.
            log::warn("content store: cannot remove {}: {}", utf16To8Lossy(path.filename().native()), ec.message());
        }
    }
    return removed;
}

StoreAssetSink::StoreAssetSink(ContentStore& store, std::filesystem::path packageRoot)
    : m_store(store)
    , m_packageRoot(std::move(packageRoot))
{
}

Result<std::filesystem::path> StoreAssetSink::resolve(std::string_view relativePath) const
{
    if (m_packageRoot.empty())
    {
        return fail(ErrorCode::InvalidArgument, "this operation has no package to read files from");
    }
    auto wide = utf8To16(relativePath);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    auto root = platform::finalPath(m_packageRoot);
    if (!root)
    {
        return std::unexpected(std::move(root.error()).withContext("open the rice folder"));
    }
    auto sourceFinal = platform::finalPath(*root / std::filesystem::path(*wide));
    if (!sourceFinal)
    {
        return std::unexpected(
            std::move(sourceFinal.error()).withContext(std::format("asset '{}'", relativePath)));
    }
    // A link inside the package must not lead outside it.
    if (!platform::isWithin(*sourceFinal, *root))
    {
        return fail(ErrorCode::AccessDenied,
                    std::format("asset '{}' resolves outside the rice folder", relativePath));
    }
    return sourceFinal;
}

Result<std::filesystem::path> StoreAssetSink::fromPackage(std::string_view relativePath)
{
    auto source = resolve(relativePath);
    if (!source)
    {
        return source;
    }
    const std::string extension = extensionOf(*source);
    // A picture is one by its bytes, not only by its name: an executable named .png went in as a wallpaper.
    if (extension == "jpg" || extension == "jpeg" || extension == "png" || extension == "bmp" || extension == "gif")
    {
        if (const std::string type = platform::sniffFileType(*source);
            type != "jpg" && type != "png" && type != "bmp" && type != "gif")
        {
            return fail(ErrorCode::InvalidData,
                        std::format("asset '{}' is not a picture (its contents are not a .{} file)", relativePath,
                                    extension));
        }
    }
    return m_store.import(*source, assetSizeLimit(extension), extension).transform_error([&](Error error) {
        return std::move(error).withContext(std::format("asset '{}'", relativePath));
    });
}

Result<std::vector<std::byte>> StoreAssetSink::readPackageFile(std::string_view relativePath,
                                                               std::uint64_t maxBytes)
{
    auto source = resolve(relativePath);
    if (!source)
    {
        return std::unexpected(std::move(source.error()));
    }
    return platform::readFileLimited(*source, maxBytes).transform_error([&](Error error) {
        return std::move(error).withContext(std::format("asset '{}'", relativePath));
    });
}

Result<std::vector<std::string>> StoreAssetSink::listPackageFolder(std::string_view relativePath)
{
    auto folder = resolve(relativePath);
    if (!folder)
    {
        return std::unexpected(std::move(folder.error()));
    }
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(*folder, ec), end; !ec && it != end; it.increment(ec))
    {
        // symlink_status: links are listed as links, not as what they point to.
        std::error_code entryError;
        if (it->symlink_status(entryError).type() != std::filesystem::file_type::regular)
        {
            continue;
        }
        // Strict, and without throwing: a name that is not valid UTF-16 (u8string() throws on it) is
        // no name a package could have produced.
        auto converted = utf16To8(it->path().filename().native());
        if (!converted)
        {
            continue;
        }
        std::string utf8 = std::move(*converted);
        if (!domain::validatePackagePath(utf8) || utf8.find('/') != std::string::npos)
        {
            continue; // names a package could not have produced are skipped
        }
        names.push_back(std::move(utf8));
        if (names.size() > limits::kPackageMaxEntries)
        {
            return fail(ErrorCode::LimitExceeded,
                        std::format("folder '{}' has too many files", relativePath));
        }
    }
    if (ec)
    {
        return fail(ErrorCode::Io, std::format("cannot list '{}': {}", relativePath, ec.message()),
                    ec.value());
    }
    std::ranges::sort(names);
    return names;
}

Result<std::filesystem::path> StoreAssetSink::fromBytes(std::span<const std::byte> bytes,
                                                        std::string_view extension)
{
    return m_store.importBytes(bytes, extension);
}

Result<std::filesystem::path> StoreAssetSink::fromSystem(const std::filesystem::path& file,
                                                         std::string_view extension)
{
    const std::string ext = extension.empty() ? extensionOf(file) : cleanExtension(extension);
    return m_store.import(file, assetSizeLimit(ext), ext);
}

} // namespace rmw::engine
