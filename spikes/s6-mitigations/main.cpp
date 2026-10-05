// Spike S6 — which process mitigation policies can rmw-core use? (docs/04, docs/06 §5)
//
// Applies one policy set (chosen on the command line), then runs the READ-ONLY operations the
// core will perform: COM apartment, IDesktopWallpaper queries, registry reads, a hidden top-level
// window with a message pump, SystemParametersInfo reads, known folders. Prints one JSON line.
//
// Usage: spike_s6_mitigations.exe [acg] [extpoints] [imageload] [mssigned] [optin]

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"
#include "rmw/platform/WinError.hpp"

#include <format>
#include <string>
#include <vector>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

namespace
{

struct Check
{
    std::string name;
    bool ok;
    std::string detail;
};

std::vector<Check> g_checks;

void record(std::string name, bool ok, std::string detail = {})
{
    g_checks.push_back({std::move(name), ok, std::move(detail)});
}

void applyPolicies(const std::vector<std::wstring>& args)
{
    for (const auto& arg : args)
    {
        BOOL ok = FALSE;
        if (arg == L"acg")
        {
            PROCESS_MITIGATION_DYNAMIC_CODE_POLICY p{};
            p.ProhibitDynamicCode = 1;
            ok = SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &p, sizeof(p));
        }
        else if (arg == L"extpoints")
        {
            PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY p{};
            p.DisableExtensionPoints = 1;
            ok = SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &p, sizeof(p));
        }
        else if (arg == L"imageload")
        {
            PROCESS_MITIGATION_IMAGE_LOAD_POLICY p{};
            p.NoRemoteImages = 1;
            p.NoLowMandatoryLabelImages = 1;
            p.PreferSystem32Images = 1;
            ok = SetProcessMitigationPolicy(ProcessImageLoadPolicy, &p, sizeof(p));
        }
        else if (arg == L"mssigned" || arg == L"optin")
        {
            PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY p{};
            if (arg == L"mssigned")
            {
                p.MicrosoftSignedOnly = 1;
            }
            else
            {
                p.MitigationOptIn = 1;
            }
            ok = SetProcessMitigationPolicy(ProcessSignaturePolicy, &p, sizeof(p));
        }
        record("policy:" + rmw::utf16To8Lossy(arg), ok != FALSE,
               ok ? "" : rmw::platform::lastError("SetProcessMitigationPolicy").toString());
    }
}

void checkDesktopWallpaper()
{
    try
    {
        auto wallpaper = wil::CoCreateInstance<IDesktopWallpaper>(CLSID_DesktopWallpaper, CLSCTX_ALL);
        UINT count = 0;
        THROW_IF_FAILED(wallpaper->GetMonitorDevicePathCount(&count));
        std::string detail = std::format("{} monitor path(s)", count);
        if (count > 0)
        {
            wil::unique_cotaskmem_string path;
            THROW_IF_FAILED(wallpaper->GetMonitorDevicePathAt(0, &path));
            wil::unique_cotaskmem_string file;
            const HRESULT hr = wallpaper->GetWallpaper(path.get(), &file);
            detail += std::format(", GetWallpaper hr=0x{:08X}", static_cast<std::uint32_t>(hr));
        }
        DESKTOP_WALLPAPER_POSITION position{};
        THROW_IF_FAILED(wallpaper->GetPosition(&position));
        record("com:IDesktopWallpaper", true, detail);
    }
    catch (const wil::ResultException& e)
    {
        record("com:IDesktopWallpaper", false, rmw::platform::hresultError(e.GetErrorCode(), "").toString());
    }
}

void checkRegistryRead()
{
    wil::unique_hkey key;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER,
                                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                                         KEY_READ, key.put());
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS read = status == ERROR_SUCCESS
                             ? RegQueryValueExW(key.get(), L"AppsUseLightTheme", nullptr, nullptr,
                                                reinterpret_cast<BYTE*>(&value), &size)
                             : status;
    record("registry:read", read == ERROR_SUCCESS || read == ERROR_FILE_NOT_FOUND,
           std::format("status={}", read));
}

void checkWindowAndPump()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{.cbSize = sizeof(wc),
                   .lpfnWndProc = DefWindowProcW,
                   .hInstance = instance,
                   .lpszClassName = L"RmwSpikeS6"};
    const ATOM atom = RegisterClassExW(&wc);
    wil::unique_hwnd window(CreateWindowExW(WS_EX_TOOLWINDOW, L"RmwSpikeS6", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                            nullptr, instance, nullptr));
    MSG msg{};
    int pumped = 0;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) && pumped < 100)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        ++pumped;
    }
    record("user32:window+pump", atom != 0 && window != nullptr);
}

void checkSystemParameters()
{
    wchar_t wallpaper[MAX_PATH]{};
    const BOOL ok = SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wallpaper, 0);
    record("spi:get-wallpaper", ok != FALSE);
}

void checkKnownFolder()
{
    wil::unique_cotaskmem_string path;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    record("shell:known-folder", SUCCEEDED(hr));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args(argv + 1, argv + argc);

    const auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    applyPolicies(args);

    checkDesktopWallpaper();
    checkRegistryRead();
    checkWindowAndPump();
    checkSystemParameters();
    checkKnownFolder();

    std::string json = "{\"checks\":[";
    bool allOk = true;
    for (std::size_t i = 0; i < g_checks.size(); ++i)
    {
        const auto& c = g_checks[i];
        allOk = allOk && c.ok;
        json += std::format("{}{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", i ? "," : "", c.name,
                            c.ok ? "true" : "false", c.detail);
    }
    json += std::format("],\"allOk\":{}}}", allOk ? "true" : "false");
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    return allOk ? 0 : 1;
}
