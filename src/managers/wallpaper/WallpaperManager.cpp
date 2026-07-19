#include <iostream>
#include <windows.h>
#include <filesystem>
#include "WallpaperManager.hpp"


bool WallpaperManager::setWallpaper(const std::filesystem::path& path) // Usando função setWallpaper do WallpaperManager
{
    if (!std::filesystem::exists(path))
    {
        return false;
    }
    
    return SystemParametersInfoW(
        SPI_SETDESKWALLPAPER,
        0,
        (void*)path.c_str(),
        SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
    );
}




