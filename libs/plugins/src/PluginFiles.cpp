#include "rmw/plugins/PluginFiles.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Images.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <format>

namespace rmw::plugins
{

Result<std::string> readPluginText(const std::filesystem::path& file, std::uint64_t maxBytes)
{
    auto bytes = platform::readFileLimited(file, maxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

Result<LoadedManifest> readPluginManifest(const std::filesystem::path& folder,
                                          domain::Diagnostics* diagnostics)
{
    auto text = readPluginText(folder / L"plugin.toml", limits::kManifestMaxBytes);
    if (!text)
    {
        return std::unexpected(std::move(text.error()).withContext("plugin.toml"));
    }
    LoadedManifest loaded;
    auto manifest = domain::parsePluginManifest(*text, loaded.diagnostics, {});
    if (diagnostics != nullptr)
    {
        *diagnostics = loaded.diagnostics;
    }
    if (!manifest)
    {
        return std::unexpected(std::move(manifest.error()));
    }
    loaded.manifest = std::move(*manifest);
    return loaded;
}

Result<std::filesystem::path> pluginImageFile(const std::filesystem::path& folder,
                                              std::string_view packagePath)
{
    auto valid = domain::validatePackagePath(packagePath);
    if (!valid)
    {
        return std::unexpected(std::move(valid.error()));
    }
    if (!domain::hasExtension(*valid, {"png", "jpg", "jpeg", "svg"}))
    {
        return fail(ErrorCode::InvalidData, "expected a .png, .jpg or .svg file");
    }
    auto root = platform::finalPath(folder);
    auto file = platform::finalPath(folder / std::filesystem::path(utf8To16Lossy(*valid)));
    if (!root || !file || !platform::isWithin(*file, *root))
    {
        return fail(ErrorCode::AccessDenied,
                    std::format("'{}' is not a file in the plugin's folder", *valid));
    }
    auto bytes = platform::readFileLimited(*file, kPluginImageMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    if (!domain::hasExtension(*valid, {"svg"}))
    {
        const auto size = domain::rasterImageSize(*bytes);
        if (!size || size->width == 0 || size->height == 0 || size->width > kPluginImageMaxSide ||
            size->height > kPluginImageMaxSide)
        {
            return fail(ErrorCode::InvalidData,
                        std::format("'{}' is not a PNG or JPEG of at most {} x {} pixels", *valid,
                                    kPluginImageMaxSide, kPluginImageMaxSide));
        }
    }
    return std::move(*file);
}

} // namespace rmw::plugins
