#include "WallpaperPlaylist.hpp"
#include "logging/Logger.hpp"
#include "../../utils/WinError.hpp"

// Stubs: the playlist feature is reimplemented in libs/modules (see docs/04-mvp.md, M1/M2).
// They only return "not done" so the project builds.

bool WallpaperPlaylist::createWallpaperPlaylist()
{
    return false;
}

bool WallpaperPlaylist::deleteWallpaperPlaylist()
{
    return false;
}

std::filesystem::path WallpaperPlaylist::allWallpaperPlaylist()
{
    return {};
}

bool WallpaperPlaylist::addToWallpaperPlaylist()
{
    return false;
}

bool WallpaperPlaylist::removeToWallpaperPlaylist()
{
    return false;
}
