#include <filesystem>
#include <array>
#include <span>

#include "StringUtils.hpp"
#include "FolderScanner.hpp"


namespace
{
    constexpr std::array<std::string_view, 10> ImageExtensions{
        ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tiff",
        ".tif", ".avif", ".heif", ".heic"
    };

    constexpr std::array<std::string_view, 5> AnimatedExtensions{
        ".gif", ".mp4", ".webm", ".apng", ".webp"
    };

    constexpr std::array<std::string_view, 1> PluginExtensions{
        ".dll"
    };

    constexpr std::array<std::string_view, 10> VideoExtensions{
        ".mp4", ".webm", ".avi", ".mkv", ".mov", ".wmv", ".flv", 
        ".m4v", ".mpeg", ".mpg"
    };

    constexpr std::array<std::string_view, 6> ThemeExtensions{
        ".json", ".yaml", ".yml", ".toml", ".ini", ".xml"
    };

    constexpr std::array<std::string_view, 17> CursorExtensions{
       ".ani", ".cur"
    };
}

std::vector<std::filesystem::path> FolderScanner::scanFolder(
            const std::filesystem::path& folderPath,
            ScanType type,
            bool recursive)
{
    // Percorrer arquivos e para cada imagem encontrada vector.push_back(path)
    // return vector
    auto extensions = setExtensions(type);
    std::vector<std::filesystem::path> files;

    if (recursive)
    {
        for(auto& entry : std::filesystem::recursive_directory_iterator(folderPath))
        {
            if(entry.is_regular_file() && 
            hasValidExtension(entry.path(), extensions))
            {
                files.push_back(entry.path());
            }
        }
    }

    else
    {
        for(auto& entry: std::filesystem::directory_iterator(folderPath))
        {
            if(entry.is_regular_file() &&
            hasValidExtension(entry.path(), extensions))
            {
                files.push_back(entry.path());
            }
        }
    }

    return files;

}


std::span<const std::string_view>
    FolderScanner::setExtensions(ScanType type)
{

switch(type)
{
    case ScanType::Images:
        return ImageExtensions;

    case ScanType::Animated:
        return AnimatedExtensions;

    case ScanType::Plugins:
        return PluginExtensions;

    case ScanType::Videos:
        return VideoExtensions;

    case ScanType::Themes:
        return ThemeExtensions;

    case ScanType::Cursors:
        return CursorExtensions;
}

return {};

}


bool FolderScanner::hasValidExtension(
    const std::filesystem::path& file,
    std::span<const std::string_view> extensions
)
{

    auto ext = StringUtils::toLower(file.extension().string());

    for (auto allowed : extensions)
    {
        if (ext == allowed)
            return true;
    }

    return false;
}


