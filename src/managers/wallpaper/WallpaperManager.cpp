#include <iostream>
#include <windows.h>
#include <filesystem>
#include <format>
#include <shobjidl.h>

#include "WallpaperManager.hpp"
#include "utils/MonitorID.hpp"
#include "utils/WinError.hpp"
#include "logging/Logger.hpp"

struct global
{
    int index = 0;
};

bool WallpaperManager::setWallpaper()
{
    global data;
    Logger& logger = Logger::getInstance();
    MonitorID monitorID;

    if (FAILED(monitorID.Initialize())) {
        logger.error(L"Failed to initialize MonitorID");
        return false;
    }

    std::cout << "\nWrite wallpaper path" << std::endl; 

    std::wstring wallpaperPath;
    std::getline(std::wcin, wallpaperPath);
    
    if (wallpaperPath.empty())
    {
        logger.error(std::format(L"Path not can be empty"));
        return 0;
    }


    if (!std::filesystem::exists(wallpaperPath))
    {
        DWORD error = GetLastError();
        logger.error(std::format(L"Error in path: {}", WinError::message(error)));
        return 0;
    }

    IDesktopWallpaper* pWallpaper = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_DesktopWallpaper,
        nullptr,
        CLSCTX_ALL,
        IID_IDesktopWallpaper,
        (void**)&pWallpaper
    );

    if (FAILED(hr))
{
    std::wcout << std::format(L"CoCreateInstance HRESULT = 0x{:08X}\n",
                              static_cast<unsigned>(hr));

    logger.error(std::format(L"Failed to create IDesktopWallpaper instance: {}",
                             WinError::message(static_cast<DWORD>(hr))));
    return 0;
}

    monitorID.ListMonitors();
    int serizalize_monitor = monitorID.ChooseMonitor(data.index);

    std::wstring devicePath = monitorID.GetDevicePath(serizalize_monitor);



    hr = pWallpaper->SetWallpaper(devicePath.c_str(), wallpaperPath.c_str());

    if (SUCCEEDED(hr))
    {
        logger.info(std::format(L"Wallpaper changed"));
    }

    else
    {   
        logger.error(std::format(L"Error: {}", WinError::message(hr)));
    }

    pWallpaper->Release();
    return 1;
}

HRESULT WallpaperManager::getWallpaper()
{
    global data;
    MonitorID monitorID;
    Logger& logger = Logger::getInstance();

    HRESULT hr = monitorID.Initialize();
    if (FAILED(hr))
    {
        logger.error(std::format(L"Error initializing monitors: {}", WinError::message(static_cast<DWORD>(hr))));
        return hr;
    }

    monitorID.ListMonitors();
    monitorID.ChooseMonitor(data.index);

    std::wstring devicePath = monitorID.GetDevicePath(static_cast<UINT>(data.index));

    if (devicePath.empty()) {
        
        logger.error(std::format(L"Monitor index {} is a invalid", data.index));
        return E_INVALIDARG;
    }

    IDesktopWallpaper* pWallpaper = nullptr;
    hr = CoCreateInstance(
        CLSID_DesktopWallpaper,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&pWallpaper)
    );

    if (FAILED(hr))
    {
        logger.error(std::format(L"Failed to create IDesktopWallpaper instance: {}", WinError::message(static_cast<DWORD>(hr))));
        return hr;
    }
    
    LPWSTR wallpaperPath = nullptr;
    hr = pWallpaper->GetWallpaper(devicePath.c_str(), &wallpaperPath);

    if (SUCCEEDED(hr) && wallpaperPath != nullptr)
    {
        std::wcout << L"\nWallpaper Path: " << wallpaperPath << std::endl;
        logger.info(std::format(L"Retrieved wallpaper: {}", wallpaperPath));
        CoTaskMemFree(wallpaperPath);
    }
    else
    {
        logger.error(std::format(L"Failed to get wallpaper for monitor {}: {}", data.index, WinError::message(static_cast<DWORD>(hr))));
    }

    pWallpaper->Release();
    return S_OK;    
}   
