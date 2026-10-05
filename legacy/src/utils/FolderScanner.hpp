#pragma once

#include <filesystem>
#include <vector>
#include <filesystem>
#include <span>
#include <string_view>


enum class ScanType
{
    Images,
    Animated,
    Videos,
    Plugins,
    Themes,

    Cursors

};


class FolderScanner
{
    public:
        std::vector<std::filesystem::path> scanFolder(
            const std::filesystem::path& folderPath,
            ScanType type,
            bool recursive = false);


    private:
        std::span<const std::string_view>
            setExtensions(ScanType type);

        bool hasValidExtension(
            const std::filesystem::path& file,
            std::span<const std::string_view> extensions);

};