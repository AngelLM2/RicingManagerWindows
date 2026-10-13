#include "rmw/base/Log.hpp"
#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <thread>
#include <vector>

namespace rmw::platform
{
namespace
{

// ---- EDID -------------------------------------------------------------------------------------

std::string descriptorText(std::span<const std::byte> descriptor)
{
    std::string text;
    for (std::size_t i = 5; i < 18; ++i)
    {
        const auto c = std::to_integer<char>(descriptor[i]);
        if (c == '\n' || c == '\r' || c == '\0')
        {
            break;
        }
        text += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }
    return text;
}

// Keys must stay printable and selector-safe: letters, digits, '-' and '_'.
std::string sanitize(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        const bool plain =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        out += plain ? c : '_';
    }
    return out;
}

// \\?\DISPLAY#DEL40F7#5&2e362cd1&0&UID256#{...} -> DISPLAY\DEL40F7\5&2e362cd1&0&UID256
std::wstring enumKeyOf(std::wstring_view devicePath)
{
    std::wstring path(devicePath);
    if (path.starts_with(LR"(\\?\)"))
    {
        path.erase(0, 4);
    }
    if (const auto guid = path.rfind(L"#{"); guid != std::wstring::npos)
    {
        path.resize(guid);
    }
    for (auto& c : path)
    {
        c = c == L'#' ? L'\\' : c;
    }
    return LR"(SYSTEM\CurrentControlSet\Enum\)" + path + LR"(\Device Parameters)";
}

EdidIdentity readEdid(std::wstring_view devicePath)
{
    std::array<std::byte, 512> buffer{};
    auto size = static_cast<DWORD>(buffer.size());
    if (RegGetValueW(HKEY_LOCAL_MACHINE, enumKeyOf(devicePath).c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr,
                     buffer.data(), &size) != ERROR_SUCCESS)
    {
        return {};
    }
    return parseEdid(std::span(buffer).first(size));
}

// ---- COM on a private STA thread -----------------------------------------------------------------

template <class T, class Work>
Result<T> onSta(Work work)
{
    Result<T> result = fail(ErrorCode::System, "the wallpaper call did not run");
    std::thread worker([&] {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init))
        {
            result = std::unexpected(hresultError(init, "CoInitializeEx"));
            return;
        }
        {
            wil::com_ptr_nothrow<IDesktopWallpaper> wallpaper;
            const HRESULT hr =
                CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(wallpaper.put()));
            result = FAILED(hr) ? Result<T>(std::unexpected(hresultError(hr, "create DesktopWallpaper")))
                                : work(*wallpaper);
        }
        CoUninitialize();
    });
    worker.join();
    return result;
}

Result<std::wstring> wide(std::string_view text)
{
    return utf8To16(text);
}

std::string narrow(const wchar_t* text)
{
    return text == nullptr ? std::string() : utf16To8Lossy(text);
}

} // namespace

// ---- identity (pure) -----------------------------------------------------------------------------

EdidIdentity parseEdid(std::span<const std::byte> edid)
{
    static constexpr std::array<std::uint8_t, 8> header = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    EdidIdentity id;
    if (edid.size() < 128)
    {
        return id;
    }
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        if (std::to_integer<std::uint8_t>(edid[i]) != header[i])
        {
            return id;
        }
    }
    const auto byte = [&](std::size_t i) { return std::to_integer<std::uint32_t>(edid[i]); };
    const std::uint32_t manufacturer = (byte(8) << 8) | byte(9);
    for (const int shift : {10, 5, 0})
    {
        const std::uint32_t letter = (manufacturer >> shift) & 0x1F;
        id.manufacturer += (letter >= 1 && letter <= 26) ? static_cast<char>('A' - 1 + letter) : '?';
    }
    id.product = static_cast<std::uint16_t>(byte(10) | (byte(11) << 8));
    id.serialNumber = byte(12) | (byte(13) << 8) | (byte(14) << 16) | (byte(15) << 24);
    for (std::size_t offset = 54; offset <= 108; offset += 18)
    {
        const auto descriptor = edid.subspan(offset, 18);
        if (byte(offset) != 0 || byte(offset + 1) != 0)
        {
            continue; // a timing descriptor
        }
        if (byte(offset + 3) == 0xFF)
        {
            id.serialText = descriptorText(descriptor);
        }
        else if (byte(offset + 3) == 0xFC)
        {
            id.name = descriptorText(descriptor);
        }
    }
    return id;
}

EdidIdentity edidFromIds(std::uint16_t manufactureId, std::uint16_t productCode)
{
    EdidIdentity id;
    const std::uint32_t manufacturer =
        static_cast<std::uint16_t>((manufactureId >> 8) | (manufactureId << 8));
    for (const int shift : {10, 5, 0})
    {
        const std::uint32_t letter = (manufacturer >> shift) & 0x1F;
        id.manufacturer += (letter >= 1 && letter <= 26) ? static_cast<char>('A' - 1 + letter) : '?';
    }
    id.product = productCode;
    return id;
}

std::string monitorIdentityKey(const EdidIdentity& edid, std::string_view devicePath, std::uint32_t targetId,
                               bool serialIsUnique)
{
    if (edid.manufacturer.empty())
    {
        // No EDID (e.g. Windows Sandbox): the device path, or the display target.
        return devicePath.empty() ? std::format("display-UID{}", targetId) : "path-" + sanitize(devicePath);
    }
    const std::string model = std::format("{}{:04X}", sanitize(edid.manufacturer), edid.product);
    const bool genericSerial = (edid.serialNumber == 0 || edid.serialNumber == 1) && edid.serialText.empty();
    if (serialIsUnique && !genericSerial)
    {
        return edid.serialText.empty()
                   ? std::format("{}-{:08X}", model, edid.serialNumber)
                   : std::format("{}-{:08X}-{}", model, edid.serialNumber, sanitize(edid.serialText));
    }
    // Twin monitors or a generic serial: the model plus the port it is plugged into.
    return std::format("{}-UID{}", model, targetId);
}

// ---- topology ---------------------------------------------------------------------------------

Result<std::vector<ports::MonitorInfo>> Win32DisplayTopology::monitors()
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 5 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
        {
            return std::unexpected(
                win32Error(static_cast<std::uint32_t>(status), "GetDisplayConfigBufferSizes"));
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                                    nullptr);
        paths.resize(pathCount);
        modes.resize(modeCount);
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "QueryDisplayConfig"));
    }

    struct Found
    {
        ports::MonitorInfo info;
        EdidIdentity edid;
        std::wstring friendly;
        std::uint32_t targetId = 0;
    };
    std::vector<Found> found;
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
        {
            continue;
        }
        const UINT32 modeIndex = path.sourceInfo.modeInfoIdx;
        if (modeIndex >= modes.size() || modes[modeIndex].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        {
            continue;
        }
        const DISPLAYCONFIG_SOURCE_MODE& source = modes[modeIndex].sourceMode;

        Found monitor;
        monitor.targetId = path.targetInfo.id;
        monitor.info.devicePath = narrow(target.monitorDevicePath);
        monitor.info.left = source.position.x;
        monitor.info.top = source.position.y;
        monitor.info.right = source.position.x + static_cast<std::int32_t>(source.width);
        monitor.info.bottom = source.position.y + static_cast<std::int32_t>(source.height);
        monitor.info.primary = source.position.x == 0 && source.position.y == 0;
        // The full EDID (with serials) lives under the monitor's device key; without a device
        // path, QueryDisplayConfig still gives manufacturer and product.
        monitor.edid = readEdid(target.monitorDevicePath);
        if (monitor.edid.manufacturer.empty() && target.flags.edidIdsValid != 0)
        {
            monitor.edid = edidFromIds(target.edidManufactureId, target.edidProductCodeId);
        }
        monitor.friendly = target.monitorFriendlyDeviceName;
        found.push_back(std::move(monitor));
    }

    std::vector<ports::MonitorInfo> monitors;
    for (std::size_t i = 0; i < found.size(); ++i)
    {
        const EdidIdentity& edid = found[i].edid;
        const auto same = std::ranges::count_if(found, [&](const Found& other) {
            return other.edid.manufacturer == edid.manufacturer && other.edid.product == edid.product &&
                   other.edid.serialNumber == edid.serialNumber && other.edid.serialText == edid.serialText;
        });
        ports::MonitorInfo info = std::move(found[i].info);
        info.identityKey = monitorIdentityKey(edid, info.devicePath, found[i].targetId, same == 1);
        // "DP" or "HDMI" says nothing: prefer the EDID name, then a descriptive fallback.
        const std::string friendly = narrow(found[i].friendly.c_str());
        info.displayName = !edid.name.empty() ? edid.name
                           : friendly.size() > 4
                               ? friendly
                               : std::format("Monitor {} ({}x{})", i + 1, info.width(), info.height());
        monitors.push_back(std::move(info));
    }
    return monitors;
}

// ---- IDesktopWallpaper --------------------------------------------------------------------------

Result<std::vector<ports::WallpaperMonitor>> Win32WallpaperApi::monitors()
{
    return onSta<std::vector<ports::WallpaperMonitor>>(
        [](IDesktopWallpaper& api) -> Result<std::vector<ports::WallpaperMonitor>> {
            UINT count = 0;
            if (const HRESULT hr = api.GetMonitorDevicePathCount(&count); FAILED(hr))
            {
                return std::unexpected(hresultError(hr, "GetMonitorDevicePathCount"));
            }
            std::vector<ports::WallpaperMonitor> monitors;
            for (UINT i = 0; i < count; ++i)
            {
                wil::unique_cotaskmem_string path;
                RECT rect{};
                // Disconnected monitors keep a path but have no rectangle.
                if (FAILED(api.GetMonitorDevicePathAt(i, path.put())) || !path ||
                    FAILED(api.GetMonitorRECT(path.get(), &rect)))
                {
                    continue;
                }
                monitors.push_back({narrow(path.get()), rect.left, rect.top, rect.right, rect.bottom});
            }
            return monitors;
        });
}

Result<std::string> Win32WallpaperApi::wallpaper(std::string_view devicePath)
{
    auto monitor = wide(devicePath);
    if (!monitor)
    {
        return std::unexpected(std::move(monitor.error()));
    }
    return onSta<std::string>([&](IDesktopWallpaper& api) -> Result<std::string> {
        wil::unique_cotaskmem_string file;
        const HRESULT hr = api.GetWallpaper(monitor->c_str(), file.put());
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "GetWallpaper"));
        }
        auto utf8 = utf16To8(file ? file.get() : L"");
        return utf8;
    });
}

Result<void> Win32WallpaperApi::setWallpaper(std::string_view devicePath, std::string_view file)
{
    auto monitor = wide(devicePath);
    auto path = wide(file);
    if (!monitor || !path)
    {
        return fail(ErrorCode::InvalidArgument, "wallpaper path is not valid UTF-8");
    }
    return onSta<void>([&](IDesktopWallpaper& api) -> Result<void> {
        const HRESULT hr = api.SetWallpaper(monitor->c_str(), path->c_str());
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "SetWallpaper"));
        }
        return {};
    });
}

Result<void> Win32WallpaperApi::setDesktopWallpaper(std::string_view file)
{
    auto path = wide(file);
    if (!path)
    {
        return fail(ErrorCode::InvalidArgument, "wallpaper path is not valid UTF-8");
    }
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path->data(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) == FALSE)
    {
        return fail(ErrorCode::System, "SPI_SETDESKWALLPAPER", GetLastError());
    }
    return {};
}

Result<ports::WallpaperPosition> Win32WallpaperApi::position()
{
    return onSta<ports::WallpaperPosition>([](IDesktopWallpaper& api) -> Result<ports::WallpaperPosition> {
        DESKTOP_WALLPAPER_POSITION position{};
        const HRESULT hr = api.GetPosition(&position);
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "GetPosition"));
        }
        return static_cast<ports::WallpaperPosition>(position);
    });
}

Result<void> Win32WallpaperApi::setPosition(ports::WallpaperPosition position)
{
    return onSta<void>([&](IDesktopWallpaper& api) -> Result<void> {
        const HRESULT hr = api.SetPosition(static_cast<DESKTOP_WALLPAPER_POSITION>(position));
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "SetPosition"));
        }
        return {};
    });
}

Result<std::uint32_t> Win32WallpaperApi::backgroundColor()
{
    return onSta<std::uint32_t>([](IDesktopWallpaper& api) -> Result<std::uint32_t> {
        COLORREF color = 0;
        const HRESULT hr = api.GetBackgroundColor(&color);
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "GetBackgroundColor"));
        }
        return (std::uint32_t{GetRValue(color)} << 16) | (std::uint32_t{GetGValue(color)} << 8) |
               GetBValue(color);
    });
}

Result<void> Win32WallpaperApi::setBackgroundColor(std::uint32_t rgb)
{
    const COLORREF color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    const auto set = [&] {
        return onSta<void>([&](IDesktopWallpaper& api) -> Result<void> {
            const HRESULT hr = api.SetBackgroundColor(color);
            if (FAILED(hr))
            {
                return std::unexpected(hresultError(hr, "SetBackgroundColor"));
            }
            return {};
        });
    };
    if (auto done = set(); !done)
    {
        return done;
    }
    // Explorer may still be finishing the picture set just before, and write the color it had over ours (the
    // fidelity suite, once: the rice's color after a restore). Read back while it settles, and set it again.
    const std::wstring wanted = std::format(L"{} {} {}", GetRValue(color), GetGValue(color), GetBValue(color));
    for (const auto wait : {std::chrono::milliseconds(400), std::chrono::milliseconds(1200)})
    {
        std::this_thread::sleep_for(wait);
        std::array<wchar_t, 32> text{};
        auto size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, LR"(Control Panel\Colors)", L"Background", RRF_RT_REG_SZ, nullptr,
                         text.data(), &size) == ERROR_SUCCESS &&
            text.data() != wanted)
        {
            log::info("wallpaper: Explorer wrote the background color over ours ({}); set again",
                      utf16To8Lossy(text.data()));
            if (auto again = set(); !again)
            {
                return again;
            }
        }
    }
    return {};
}

Result<ports::SlideshowState> Win32WallpaperApi::slideshow()
{
    return onSta<ports::SlideshowState>([](IDesktopWallpaper& api) -> Result<ports::SlideshowState> {
        ports::SlideshowState state;
        DESKTOP_SLIDESHOW_STATE status{};
        if (const HRESULT hr = api.GetStatus(&status); FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "GetStatus"));
        }
        // DSS_SLIDESHOW, not DSS_ENABLED (ADR-0004 decision 5).
        state.active = (static_cast<unsigned>(status) & DSS_SLIDESHOW) != 0;
        if (!state.active)
        {
            return state;
        }
        wil::com_ptr_nothrow<IShellItemArray> items;
        if (const HRESULT hr = api.GetSlideshow(items.put()); FAILED(hr) || !items)
        {
            return std::unexpected(hresultError(FAILED(hr) ? hr : E_POINTER, "GetSlideshow"));
        }
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i)
        {
            wil::com_ptr_nothrow<IShellItem> item;
            wil::unique_cotaskmem_string path;
            if (SUCCEEDED(items->GetItemAt(i, item.put())) && item &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put())) && path)
            {
                state.items.push_back(narrow(path.get()));
            }
        }
        DESKTOP_SLIDESHOW_OPTIONS options{};
        UINT tick = 0;
        if (SUCCEEDED(api.GetSlideshowOptions(&options, &tick)))
        {
            state.shuffle = (static_cast<unsigned>(options) & DSO_SHUFFLEIMAGES) != 0;
            state.intervalMs = tick;
        }
        return state;
    });
}

Result<void> Win32WallpaperApi::setSlideshow(const ports::SlideshowState& state)
{
    std::vector<std::wstring> paths;
    for (const auto& item : state.items)
    {
        auto path = wide(item);
        if (!path)
        {
            return std::unexpected(std::move(path.error()));
        }
        paths.push_back(std::move(*path));
    }
    if (paths.empty())
    {
        return fail(ErrorCode::InvalidArgument, "a slideshow needs at least one image");
    }
    return onSta<void>([&](IDesktopWallpaper& api) -> Result<void> {
        // PIDLs are __unaligned pointers: keep them out of std containers (C4090) and free them
        // with the shell allocator on every exit path.
        const auto pidls = std::make_unique<PCIDLIST_ABSOLUTE[]>(paths.size());
        std::size_t parsed = 0;
        const ScopeExit freePidls([&]() noexcept {
            for (std::size_t i = 0; i < parsed; ++i)
            {
                CoTaskMemFree(const_cast<ITEMIDLIST*>(reinterpret_cast<const ITEMIDLIST*>(pidls[i])));
            }
        });
        for (const auto& path : paths)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (const HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr); FAILED(hr))
            {
                return std::unexpected(hresultError(hr, "resolve slideshow image"));
            }
            pidls[parsed++] = pidl;
        }
        wil::com_ptr_nothrow<IShellItemArray> array;
        HRESULT hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(parsed), pidls.get(), array.put());
        if (SUCCEEDED(hr))
        {
            hr = api.SetSlideshow(array.get());
        }
        if (SUCCEEDED(hr))
        {
            hr = api.SetSlideshowOptions(state.shuffle ? DSO_SHUFFLEIMAGES
                                                       : static_cast<DESKTOP_SLIDESHOW_OPTIONS>(0),
                                         state.intervalMs);
        }
        if (FAILED(hr))
        {
            return std::unexpected(hresultError(hr, "SetSlideshow"));
        }
        return {};
    });
}

Result<std::string> Win32WallpaperApi::transcodedWallpaper()
{
    wil::unique_cotaskmem_string appData;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, appData.put())))
    {
        return fail(ErrorCode::System, "cannot find %APPDATA%");
    }
    return utf16To8(std::wstring(appData.get()) + LR"(\Microsoft\Windows\Themes\TranscodedWallpaper)");
}

std::string Win32WallpaperApi::defaultWallpaper()
{
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    const std::wstring folder =
        length > 0 && length < windows.size() ? std::wstring(windows.data(), length) : std::wstring(L"C:\\Windows");
    return utf16To8Lossy(folder + LR"(\Web\Wallpaper\Windows\img0.jpg)");
}

Result<std::string> Win32WallpaperApi::sniffImageType(std::string_view file)
{
    auto path = wide(file);
    if (!path)
    {
        return std::string();
    }
    const wil::unique_hfile handle(CreateFileW(path->c_str(), GENERIC_READ,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle)
    {
        return std::string(); // missing or unreadable
    }
    std::array<unsigned char, 12> magic{};
    DWORD read = 0;
    if (ReadFile(handle.get(), magic.data(), static_cast<DWORD>(magic.size()), &read, nullptr) == FALSE ||
        read < 4)
    {
        return std::string();
    }
    // Every picture Windows takes as a wallpaper: a .tif, .webp or .heic one counted as "gone" and its
    // lower-quality TranscodedWallpaper was kept in its place (v1.8).
    if ((magic[0] == 'I' && magic[1] == 'I' && magic[2] == 0x2A && magic[3] == 0x00) ||
        (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0x00 && magic[3] == 0x2A))
    {
        return std::string("tif");
    }
    if (magic[0] == 'I' && magic[1] == 'I' && magic[2] == 0xBC)
    {
        return std::string("jxr");
    }
    if (read >= 12 && magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F' && magic[8] == 'W' &&
        magic[9] == 'E' && magic[10] == 'B' && magic[11] == 'P')
    {
        return std::string("webp");
    }
    if (read >= 12 && magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p')
    {
        const std::string brand(reinterpret_cast<const char*>(magic.data()) + 8, 4);
        if (brand == "avif" || brand == "avis")
        {
            return std::string("avif");
        }
        if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1" || brand == "hevc")
        {
            return std::string("heic");
        }
    }
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF)
    {
        return std::string("jpg");
    }
    if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
    {
        return std::string("png");
    }
    if (magic[0] == 'B' && magic[1] == 'M')
    {
        return std::string("bmp");
    }
    if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F')
    {
        return std::string("gif");
    }
    return std::string();
}

bool Win32WallpaperApi::isWindowsFile(std::string_view file)
{
    auto path = wide(file);
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    return path && length > 0 && isWithin(*path, std::wstring(windows.data(), length));
}

} // namespace rmw::platform
