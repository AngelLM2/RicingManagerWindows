// Spike S5 — monitor identity and wallpaper snapshot fidelity (docs/04, docs/03 §6).
//
//   spike_s5.exe monitors   READ-ONLY, safe on the host: lists monitors through IDesktopWallpaper
//                           and QueryDisplayConfig, reads EDID identity from the registry.
//   spike_s5.exe snapshot   WINDOWS SANDBOX ONLY: captures the wallpaper state, changes it and
//                           restores it (single image incl. TranscodedWallpaper, slideshow).
// Prints one JSON object.

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <ole2.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

std::vector<std::string> g_items;

std::string jsonEscape(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

void item(const std::string& name, bool ok, const std::string& detail)
{
    g_items.push_back(std::format("{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", jsonEscape(name),
                                  ok ? "true" : "false", jsonEscape(detail)));
}

std::string narrow(std::wstring_view text)
{
    return rmw::utf16To8Lossy(text);
}

// ---------------------------------------------------------------------------------------------
// Part 1: monitor identity
// ---------------------------------------------------------------------------------------------

struct EdidIdentity
{
    bool found = false;
    std::string manufacturer; // 3-letter PNP id
    unsigned product = 0;
    unsigned serialNumber = 0; // numeric serial (bytes 12-15)
    std::string serialText;    // descriptor 0xFF
    std::string name;          // descriptor 0xFC
};

std::string descriptorText(const BYTE* d)
{
    std::string text(reinterpret_cast<const char*>(d + 5), 13);
    const auto end = text.find_first_of("\n\r");
    if (end != std::string::npos)
    {
        text.resize(end);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }
    return text;
}

// \\?\DISPLAY#DEL40F7#5&2e362cd1&0&UID256#{e6f07b5f-...} -> DISPLAY\DEL40F7\5&2e362cd1&0&UID256
EdidIdentity readEdid(std::wstring_view devicePath)
{
    EdidIdentity id;
    std::wstring path(devicePath);
    if (path.rfind(L"\\\\?\\", 0) == 0)
    {
        path.erase(0, 4);
    }
    const auto guid = path.rfind(L"#{");
    if (guid != std::wstring::npos)
    {
        path.resize(guid);
    }
    for (auto& ch : path)
    {
        if (ch == L'#')
        {
            ch = L'\\';
        }
    }
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + path + L"\\Device Parameters";

    std::array<BYTE, 512> edid{};
    DWORD size = static_cast<DWORD>(edid.size());
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, edid.data(),
                     &size) != ERROR_SUCCESS ||
        size < 128)
    {
        return id;
    }
    id.found = true;
    const unsigned mfg = (edid[8] << 8) | edid[9];
    id.manufacturer = {static_cast<char>('A' - 1 + ((mfg >> 10) & 0x1F)),
                       static_cast<char>('A' - 1 + ((mfg >> 5) & 0x1F)),
                       static_cast<char>('A' - 1 + (mfg & 0x1F))};
    id.product = edid[10] | (edid[11] << 8);
    id.serialNumber = edid[12] | (edid[13] << 8) | (edid[14] << 16) | (static_cast<unsigned>(edid[15]) << 24);
    for (int offset = 54; offset <= 108; offset += 18)
    {
        const BYTE* d = edid.data() + offset;
        if (d[0] == 0 && d[1] == 0 && d[3] == 0xFF)
        {
            id.serialText = descriptorText(d);
        }
        else if (d[0] == 0 && d[1] == 0 && d[3] == 0xFC)
        {
            id.name = descriptorText(d);
        }
    }
    return id;
}

int runMonitors()
{
    auto wallpaper = wil::CoCreateInstance<IDesktopWallpaper>(CLSID_DesktopWallpaper, CLSCTX_ALL);
    UINT count = 0;
    wallpaper->GetMonitorDevicePathCount(&count);
    std::vector<std::wstring> wallpaperPaths;
    for (UINT i = 0; i < count; ++i)
    {
        wil::unique_cotaskmem_string path;
        if (SUCCEEDED(wallpaper->GetMonitorDevicePathAt(i, &path)))
        {
            RECT rect{};
            const HRESULT rectHr = wallpaper->GetMonitorRECT(path.get(), &rect);
            item(std::format("wallpaper-api:monitor{}", i), true,
                 std::format("{} rect={}x{}@{},{} attached={}", narrow(path.get()), rect.right - rect.left,
                             rect.bottom - rect.top, rect.left, rect.top, SUCCEEDED(rectHr)));
            wallpaperPaths.emplace_back(path.get());
        }
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    paths.resize(pathCount);

    int matched = 0;
    for (const auto& p : paths)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = p.targetInfo.adapterId;
        target.header.id = p.targetInfo.id;
        DisplayConfigGetDeviceInfo(&target.header);

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        DisplayConfigGetDeviceInfo(&source.header);

        bool inWallpaperApi = false;
        for (const auto& wp : wallpaperPaths)
        {
            if (CompareStringOrdinal(wp.c_str(), -1, target.monitorDevicePath, -1, TRUE) == CSTR_EQUAL)
            {
                inWallpaperApi = true;
            }
        }
        matched += inWallpaperApi ? 1 : 0;

        const EdidIdentity edid = readEdid(target.monitorDevicePath);
        item(std::format("displayconfig:{}", narrow(source.viewGdiDeviceName)), inWallpaperApi,
             std::format(
                 "friendly='{}' path={} matchesWallpaperApi={} edidFromConfig={:04X}/{:04X} "
                 "connector={} | registryEDID found={} id={}-{:04X} serial={} serialText='{}' name='{}'",
                 narrow(target.monitorFriendlyDeviceName), narrow(target.monitorDevicePath), inWallpaperApi,
                 target.edidManufactureId, target.edidProductCodeId, target.connectorInstance, edid.found,
                 edid.manufacturer, edid.product, edid.serialNumber, edid.serialText, edid.name));
    }
    item("summary", matched == static_cast<int>(paths.size()),
         std::format("{} active display path(s), {} matched IDesktopWallpaper paths", paths.size(), matched));
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Part 2: wallpaper snapshot fidelity (sandbox only)
// ---------------------------------------------------------------------------------------------

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &size) ==
        ERROR_SUCCESS)
    {
        return buffer;
    }
    return {};
}

DWORD regDword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD fallback)
{
    DWORD data = 0;
    DWORD size = sizeof(data);
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS
               ? data
               : fallback;
}

// Writes a solid-color 64x64 24-bit BMP.
void writeBmp(const fs::path& path, BYTE r, BYTE g, BYTE b)
{
    constexpr int side = 64;
    constexpr int rowBytes = side * 3;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = side;
    info.biHeight = side;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biSizeImage = rowBytes * side;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int i = 0; i < side * side; ++i)
    {
        const char pixel[3] = {static_cast<char>(b), static_cast<char>(g), static_cast<char>(r)};
        out.write(pixel, 3);
    }
}

std::wstring currentWallpaper(IDesktopWallpaper* wp, const wchar_t* monitor)
{
    wil::unique_cotaskmem_string file;
    if (SUCCEEDED(wp->GetWallpaper(monitor, &file)) && file)
    {
        return file.get();
    }
    return {};
}

unsigned st(DESKTOP_SLIDESHOW_STATE s)
{
    return static_cast<unsigned>(s);
}

void waitMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

constexpr auto kDesktopKey = L"Control Panel\\Desktop";
constexpr auto kWallpapersKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";

int runSnapshot()
{
    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
    {
        waitMs(1000);
    }
    waitMs(4000);

    auto wp = wil::CoCreateInstance<IDesktopWallpaper>(CLSID_DesktopWallpaper, CLSCTX_ALL);
    wil::unique_cotaskmem_string monitor;
    wp->GetMonitorDevicePathAt(0, &monitor);

    const fs::path work = fs::temp_directory_path() / "rmw-s5";
    fs::create_directories(work / "store");
    fs::create_directories(work / "slides");

    // --- capture --------------------------------------------------------------------------
    const std::wstring originalFile = currentWallpaper(wp.get(), monitor.get());
    const std::wstring regWallpaper = regString(HKEY_CURRENT_USER, kDesktopKey, L"WallPaper");
    const DWORD backgroundType = regDword(HKEY_CURRENT_USER, kWallpapersKey, L"BackgroundType", 99);
    DESKTOP_WALLPAPER_POSITION position{};
    wp->GetPosition(&position);
    COLORREF background = 0;
    wp->GetBackgroundColor(&background);
    DESKTOP_SLIDESHOW_STATE slideState{};
    wp->GetStatus(&slideState);

    wchar_t appData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    const fs::path transcoded = fs::path(appData) / L"Microsoft\\Windows\\Themes\\TranscodedWallpaper";
    const bool transcodedExists = fs::exists(transcoded);

    item(
        "capture", true,
        std::format(
            "GetWallpaper='{}' reg.WallPaper='{}' BackgroundType={} position={} bg=0x{:06X} slideshow=0x{:X} "
            "TranscodedWallpaper exists={} size={}",
            narrow(originalFile), narrow(regWallpaper), backgroundType, static_cast<int>(position),
            background, st(slideState), transcodedExists, transcodedExists ? fs::file_size(transcoded) : 0));

    // The store keeps a copy of whatever file is displayed now (TranscodedWallpaper is overwritten
    // by the next SetWallpaper, so it must be copied BEFORE changing anything).
    const fs::path source =
        !originalFile.empty() && fs::exists(originalFile) ? fs::path(originalFile) : transcoded;
    const fs::path stored = work / "store" / "original-wallpaper.img";
    std::error_code ec;
    fs::copy_file(source, stored, fs::copy_options::overwrite_existing, ec);
    item("store:copy-original", !ec, std::format("from='{}' {}", narrow(source.wstring()), ec.message()));

    // --- change -----------------------------------------------------------------------------
    const fs::path red = work / "red.bmp";
    writeBmp(red, 220, 30, 30);
    const HRESULT setHr = wp->SetWallpaper(monitor.get(), red.c_str());
    waitMs(2000);
    const bool transcodedChanged =
        fs::exists(transcoded) && transcodedExists && fs::last_write_time(transcoded) != fs::file_time_type{};
    item("change:set-red", SUCCEEDED(setHr) && currentWallpaper(wp.get(), monitor.get()) == red.wstring(),
         std::format("hr=0x{:08X} now='{}' transcodedRewritten={}", static_cast<unsigned>(setHr),
                     narrow(currentWallpaper(wp.get(), monitor.get())), transcodedChanged));

    // --- restore single image from the store ---------------------------------------------------
    const HRESULT restoreHr = wp->SetWallpaper(monitor.get(), stored.c_str());
    wp->SetPosition(position);
    wp->SetBackgroundColor(background);
    waitMs(2000);
    item("restore:image-from-store",
         SUCCEEDED(restoreHr) && currentWallpaper(wp.get(), monitor.get()) == stored.wstring(),
         std::format("hr=0x{:08X} now='{}' BackgroundType={}", static_cast<unsigned>(restoreHr),
                     narrow(currentWallpaper(wp.get(), monitor.get())),
                     regDword(HKEY_CURRENT_USER, kWallpapersKey, L"BackgroundType", 99)));

    // --- slideshow round trip -----------------------------------------------------------------
    writeBmp(work / "slides" / "a.bmp", 30, 200, 30);
    writeBmp(work / "slides" / "b.bmp", 30, 30, 200);
    wil::com_ptr<IShellItem> folder;
    SHCreateItemFromParsingName((work / "slides").c_str(), nullptr, IID_PPV_ARGS(&folder));
    wil::com_ptr<IShellItemArray> items;
    SHCreateShellItemArrayFromShellItem(folder.get(), IID_PPV_ARGS(&items));
    const HRESULT slideHr = wp->SetSlideshow(items.get());
    wp->SetSlideshowOptions(DSO_SHUFFLEIMAGES, 60000);
    waitMs(2500);

    DESKTOP_SLIDESHOW_STATE stateOn{};
    wp->GetStatus(&stateOn);
    wil::com_ptr<IShellItemArray> captured;
    const HRESULT getSlidesHr = wp->GetSlideshow(&captured);
    DESKTOP_SLIDESHOW_OPTIONS options{};
    UINT tick = 0;
    wp->GetSlideshowOptions(&options, &tick);
    DWORD capturedCount = 0;
    if (captured)
    {
        captured->GetCount(&capturedCount);
    }
    const DWORD typeDuringSlideshow = regDword(HKEY_CURRENT_USER, kWallpapersKey, L"BackgroundType", 99);
    item("slideshow:set-and-capture",
         SUCCEEDED(slideHr) && (st(stateOn) & DSS_ENABLED) && SUCCEEDED(getSlidesHr),
         std::format("hr=0x{:08X} state=0x{:X} items={} options=0x{:X} tick={} BackgroundType={}",
                     static_cast<unsigned>(slideHr), st(stateOn), capturedCount, static_cast<int>(options),
                     tick, typeDuringSlideshow));

    // Switch to a static image, then restore the captured slideshow.
    wp->SetWallpaper(monitor.get(), red.c_str());
    waitMs(2000);
    DESKTOP_SLIDESHOW_STATE stateOff{};
    wp->GetStatus(&stateOff);
    const HRESULT reSlideHr = captured ? wp->SetSlideshow(captured.get()) : E_POINTER;
    wp->SetSlideshowOptions(options, tick);
    waitMs(2500);
    DESKTOP_SLIDESHOW_STATE stateRestored{};
    wp->GetStatus(&stateRestored);
    DESKTOP_SLIDESHOW_OPTIONS optionsRestored{};
    UINT tickRestored = 0;
    wp->GetSlideshowOptions(&optionsRestored, &tickRestored);
    item("slideshow:restore",
         SUCCEEDED(reSlideHr) && !(st(stateOff) & DSS_ENABLED) && (st(stateRestored) & DSS_ENABLED) &&
             optionsRestored == options && tickRestored == tick,
         std::format("off=0x{:X} restored=0x{:X} options={} tick={}", st(stateOff), st(stateRestored),
                     static_cast<int>(optionsRestored), tickRestored));

    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    const std::wstring_view mode = argc > 1 ? argv[1] : L"monitors";
    try
    {
        if (mode == L"snapshot")
        {
            runSnapshot();
        }
        else
        {
            runMonitors();
        }
    }
    catch (const std::exception& e)
    {
        item("exception", false, e.what());
    }

    std::string json = "{\"items\":[";
    for (std::size_t i = 0; i < g_items.size(); ++i)
    {
        json += (i ? "," : "") + g_items[i];
    }
    json += "]}";
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    return 0;
}
