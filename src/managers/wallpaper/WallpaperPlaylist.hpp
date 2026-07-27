#pragma once

#include <filesystem>


class WallpaperPlaylist
{
    bool createWallpaperPlaylist();

    bool deleteWallpaperPlaylist();

    std::filesystem::path allWallpaperPlaylist();

    bool addToWallpaperPlaylist();

    bool removeToWallpaperPlaylist();

}; 