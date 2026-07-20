#include <iostream>
#include <windows.h>
#include <format>

#include "managers/wallpaper/WallpaperManager.hpp"
#include "App.hpp"
#include "utils/WinError.hpp"
#include "logging/Logger.hpp"

#pragma comment(lib, "ole32.lib")

void App::run()
{
    Logger& logger = Logger::getInstance();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        logger.error(std::format(L"Failed to initialize COM: {}", hr));
        return;
    }

    WallpaperManager wp;

    wp.setWallpaper();
    wp.getWallpaper();

    CoUninitialize();
}