#pragma once

#include <filesystem>

class WallpaperManager
{
    public:
        bool setWallpaper(const std::filesystem::path& path);

};
