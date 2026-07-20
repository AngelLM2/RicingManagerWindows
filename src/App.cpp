#include <iostream>
#include <windows.h>
#include <format>

#include "managers/wallpaper/WallpaperManager.hpp"
#include "App.hpp"
#include "utils/WinError.hpp"
#include "logging/Logger.hpp"

void App::run()
{
    // Definir
    WallpaperManager wp;

    // Logger
    Logger& logger = Logger::getInstance();

    // Wallpapers
    wp.setWallpaper();
    wp.getWallpaper();
}