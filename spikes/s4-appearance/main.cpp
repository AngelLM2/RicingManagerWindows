// Spike S4 — live light/dark mode and accent color (docs/04, docs/09 §6).
// RUN ONLY INSIDE WINDOWS SANDBOX: it changes the real theme.
//
//   spike_s4.exe [toggles] [mitigations]
//
// A. Writes a managed .theme (SystemMode/AppMode/ColorizationColor) and applies it with the
//    undocumented IThemeManager2 (interface from Auto Dark Mode / namazso, MIT), ignoring
//    wallpaper/cursors/icons/sounds/screensaver. Repeats `toggles` times alternating
//    dark/light and accent colors; verifies the registry; measures the time per apply.
// B. Registry-only fallback (AppsUseLightTheme/SystemUsesLightTheme + broadcast).
// C. If "mitigations" is given, applies the S6 policy set first (validates the apply helper).

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <shlobj.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <ole2.h>
#include <oleauto.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// --- IThemeManager2 (undocumented) ---------------------------------------------------------------
MIDL_INTERFACE("26e4185f-0528-475f-acaf-abe89ba6017d")
ITheme : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(BSTR * name) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DisplayName(BSTR name) = 0;
};

MIDL_INTERFACE("c1e8c83e-845d-4d95-81db-e283fdffc000")
IThemeManager2 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Init(int flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE InitAsync(HWND hwnd, int unk) = 0;
    virtual HRESULT STDMETHODCALLTYPE Refresh() = 0;
    virtual HRESULT STDMETHODCALLTYPE RefreshAsync(HWND hwnd, int unk) = 0;
    virtual HRESULT STDMETHODCALLTYPE RefreshComplete() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetThemeCount(int* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTheme(int index, ITheme** theme) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsThemeDisabled(int index, int* disabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentTheme(HWND parent, int index, int applyNow, ULONG applyFlags,
                                                      ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCustomTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateThemePack(HWND hwnd, BSTR unk1, ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE CloneAndSetCurrentTheme(HWND hwnd, BSTR unk1, BSTR * unk2) = 0;
    virtual HRESULT STDMETHODCALLTYPE InstallThemePack(HWND hwnd, BSTR unk1, int unk2, ULONG packFlags,
                                                       BSTR* unk3, ITheme** unk4) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteTheme(BSTR unk1) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenTheme(HWND hwnd, BSTR path, ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddAndSelectTheme(HWND hwnd, BSTR path, ULONG applyFlags,
                                                        ULONG packFlags) = 0;
};

constexpr CLSID kClsidThemeManager2 = {
    0x9324da94, 0x50ec, 0x4a14, {0xa7, 0x70, 0xe9, 0x0c, 0xa0, 0x3e, 0x7c, 0x8f}};

// Legacy IThemeManager (themeui.dll): applies a .theme by path, ignoring apply flags.
MIDL_INTERFACE("D23CC733-5522-406D-8DFB-B3CF5EF52A71")
ILegacyTheme : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(BSTR * name) = 0;
};

MIDL_INTERFACE("0646EBBE-C1B7-4045-8FD0-FFD65D3FC792")
ILegacyThemeManager : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_CurrentTheme(ILegacyTheme * *theme) = 0;
    virtual HRESULT STDMETHODCALLTYPE ApplyTheme(BSTR path) = 0;
};

constexpr CLSID kClsidLegacyThemeManager = {
    0xC04B329E, 0x5823, 0x4415, {0x9C, 0x93, 0xBA, 0x44, 0x68, 0x89, 0x47, 0xB0}};

constexpr ULONG kIgnoreBackground = 1u << 0;
constexpr ULONG kIgnoreCursor = 1u << 1;
constexpr ULONG kIgnoreDesktopIcons = 1u << 2;
constexpr ULONG kIgnoreSound = 1u << 4;
constexpr ULONG kIgnoreScreensaver = 1u << 5;
constexpr ULONG kNoHourglass = 1u << 8;
constexpr ULONG kPackSilent = 1u << 2;

// --------------------------------------------------------------------------------------------------

namespace
{

std::vector<std::string> g_items;

void item(const std::string& name, bool ok, const std::string& detail)
{
    std::string safe;
    for (char c : detail)
    {
        safe += (c == '"' || c == '\\') ? '/' : c;
    }
    g_items.push_back(
        std::format("{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", name, ok ? "true" : "false", safe));
}

constexpr auto kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr auto kDwm = L"Software\\Microsoft\\Windows\\DWM";

DWORD regDword(const wchar_t* key, const wchar_t* name, DWORD fallback = 0xFFFFFFFF)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
                   ERROR_SUCCESS
               ? value
               : fallback;
}

struct Look
{
    bool dark;
    COLORREF rgb; // 0x00BBGGRR like COLORREF
};

std::uint32_t argbFromRgb(COLORREF rgb)
{
    return 0xC4000000u | (GetRValue(rgb) << 16) | (GetGValue(rgb) << 8) | GetBValue(rgb);
}

// Builds the managed theme from a valid Windows theme (aero.theme here; the product will use the
// currently active theme so cursors, sounds and wallpaper stay as they are) and replaces only the
// keys we manage. Written as plain ASCII like the files Windows ships.
fs::path writeTheme(const fs::path& dir, int index, const Look& look)
{
    (void)index;
    wchar_t windowsDir[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    std::ifstream in(fs::path(windowsDir) / L"Resources" / L"Themes" / L"aero.theme", std::ios::binary);
    std::string line;
    std::string out;
    const std::string mode = look.dark ? "Dark" : "Light";
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.rfind("DisplayName=", 0) == 0)
        {
            line = "DisplayName=RMW S4 Managed";
        }
        else if (line.rfind("ColorizationColor=", 0) == 0)
        {
            line = std::format("ColorizationColor=0X{:08X}", argbFromRgb(look.rgb));
        }
        else if (line.rfind("SystemMode=", 0) == 0)
        {
            line = "SystemMode=" + mode;
        }
        else if (line.rfind("AppMode=", 0) == 0)
        {
            line = "AppMode=" + mode;
        }
        out += line + "\r\n";
    }
    const fs::path path = dir / L"rmw-s4-managed.theme";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return path;
}

bool registryMatches(const Look& look, std::string& seen)
{
    const DWORD apps = regDword(kPersonalize, L"AppsUseLightTheme");
    const DWORD system = regDword(kPersonalize, L"SystemUsesLightTheme");
    const DWORD colorization = regDword(kDwm, L"ColorizationColor");
    const bool modeOk = apps == (look.dark ? 0u : 1u) && system == (look.dark ? 0u : 1u);
    const bool colorOk = (colorization & 0x00FFFFFF) == (argbFromRgb(look.rgb) & 0x00FFFFFF);
    seen = std::format("apps={} sys={} colorization=0x{:08X}", apps, system, colorization);
    return modeOk && colorOk;
}

void applyMitigations()
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY acg{};
    acg.ProhibitDynamicCode = 1;
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY image{};
    image.NoRemoteImages = 1;
    image.NoLowMandatoryLabelImages = 1;
    image.PreferSystem32Images = 1;
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig{};
    sig.MicrosoftSignedOnly = 1;
    const bool ok = SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &acg, sizeof(acg)) &&
                    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext)) &&
                    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &image, sizeof(image)) &&
                    SetProcessMitigationPolicy(ProcessSignaturePolicy, &sig, sizeof(sig));
    item("mitigations:applied", ok,
         ok ? "acg+extpoints+imageload+mssigned" : "SetProcessMitigationPolicy failed");
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
    {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

// Auto Dark Mode's sequence: fresh manager (re-reads the Themes folders), find the managed theme
// by display name, apply it by index with SetCurrentTheme.
HRESULT applyByIndex(const wchar_t* displayName, ULONG applyFlags, std::string& detail)
{
    wil::com_ptr<IThemeManager2> manager;
    HRESULT hr = CoCreateInstance(kClsidThemeManager2, nullptr, CLSCTX_ALL, __uuidof(IThemeManager2),
                                  manager.put_void());
    if (FAILED(hr))
    {
        detail = "create";
        return hr;
    }
    if (FAILED(hr = manager->Init(0)))
    {
        detail = "init";
        return hr;
    }
    int count = 0;
    if (FAILED(hr = manager->GetThemeCount(&count)))
    {
        detail = "count";
        return hr;
    }
    for (int i = 0; i < count; ++i)
    {
        wil::com_ptr<ITheme> theme;
        if (FAILED(manager->GetTheme(i, theme.put())) || !theme)
        {
            continue;
        }
        wil::unique_bstr name;
        if (SUCCEEDED(theme->get_DisplayName(name.put())) && name && wcscmp(name.get(), displayName) == 0)
        {
            hr = manager->SetCurrentTheme(nullptr, i, 1, applyFlags, 0);
            detail = std::format("idx={}/{}", i, count);
            return hr;
        }
    }
    std::string names;
    for (int i = 0; i < count; ++i)
    {
        wil::com_ptr<ITheme> theme;
        wil::unique_bstr name;
        if (SUCCEEDED(manager->GetTheme(i, theme.put())) && theme &&
            SUCCEEDED(theme->get_DisplayName(name.put())) && name)
        {
            names += rmw::utf16To8Lossy(name.get()) + "|";
        }
    }
    detail = std::format("not found among {} themes: {}", count, names);
    return E_FAIL;
}

void runThemeManager(int toggles)
{
    wchar_t localAppData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    const fs::path themes = fs::path(localAppData) / L"Microsoft" / L"Windows" / L"Themes";
    fs::create_directories(themes);

    const COLORREF palette[] = {RGB(0x7A, 0xA2, 0xF7), RGB(0xF7, 0x76, 0x8E), RGB(0x2C, 0xD5, 0xC4),
                                RGB(0xBB, 0x9A, 0xF7)};
    constexpr ULONG applyFlags = kIgnoreBackground | kIgnoreCursor | kIgnoreDesktopIcons | kIgnoreSound |
                                 kIgnoreScreensaver | kNoHourglass;

    // First use: the managed theme is not in IThemeManager2's list yet. Apply it once through the
    // legacy IThemeManager (ignores flags), then learn the name Windows lists it under.
    std::wstring learnedName = L"RMW S4 Managed";
    {
        const Look first{.dark = false, .rgb = palette[3]};
        const fs::path path = writeTheme(themes, 0, first);
        std::string detail;
        if (applyByIndex(learnedName.c_str(), applyFlags, detail) == E_FAIL)
        {
            const auto start = std::chrono::steady_clock::now();
            wil::com_ptr<ILegacyThemeManager> legacy;
            HRESULT hr = CoCreateInstance(kClsidLegacyThemeManager, nullptr, CLSCTX_ALL,
                                          __uuidof(ILegacyThemeManager), legacy.put_void());
            const wil::unique_bstr bstr(SysAllocString(path.c_str()));
            if (SUCCEEDED(hr))
            {
                hr = legacy->ApplyTheme(bstr.get());
            }
            std::string seen;
            bool applied = false;
            for (int wait = 0; wait < 100 && SUCCEEDED(hr) && !applied; ++wait)
            {
                applied = registryMatches(first, seen);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

            wil::com_ptr<IThemeManager2> manager;
            int current = -1;
            if (SUCCEEDED(CoCreateInstance(kClsidThemeManager2, nullptr, CLSCTX_ALL, __uuidof(IThemeManager2),
                                           manager.put_void())) &&
                SUCCEEDED(manager->Init(0)) && SUCCEEDED(manager->GetCurrentTheme(&current)))
            {
                wil::com_ptr<ITheme> theme;
                wil::unique_bstr name;
                if (SUCCEEDED(manager->GetTheme(current, theme.put())) && theme &&
                    SUCCEEDED(theme->get_DisplayName(name.put())) && name)
                {
                    learnedName = name.get();
                }
            }
            item("legacy:register-managed-theme", SUCCEEDED(hr) && applied,
                 std::format(
                     "hr=0x{:08X} applied={} in {:.0f}ms; IThemeManager2 now lists it as '{}' (idx {}) {}",
                     static_cast<unsigned>(hr), applied, ms, rmw::utf16To8Lossy(learnedName), current, seen));
        }
    }

    int succeeded = 0;
    std::vector<double> durations;
    std::string firstFailure;
    for (int i = 0; i < toggles; ++i)
    {
        const Look look{.dark = (i % 2) == 0, .rgb = palette[i % 4]};
        writeTheme(themes, i, look);

        const auto start = std::chrono::steady_clock::now();
        std::string detail;
        const HRESULT hr = applyByIndex(learnedName.c_str(), applyFlags, detail);

        // Consider it applied once the registry reflects the look (max 3 s).
        bool ok = false;
        std::string seen;
        for (int wait = 0; wait < 60 && SUCCEEDED(hr); ++wait)
        {
            if (registryMatches(look, seen))
            {
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        durations.push_back(ms);
        if (ok)
        {
            ++succeeded;
        }
        else if (firstFailure.empty())
        {
            firstFailure = std::format("#{} hr=0x{:08X} {} {}", i, static_cast<unsigned>(hr), detail, seen);
        }
    }

    const double rate = toggles ? 100.0 * succeeded / toggles : 0;
    item("tm2:toggles", rate >= 99.0,
         std::format("{}/{} ok ({:.1f}%) avg={:.0f}ms p50={:.0f}ms p95={:.0f}ms max={:.0f}ms firstFailure={}",
                     succeeded, toggles, rate,
                     durations.empty()
                         ? 0.0
                         : std::accumulate(durations.begin(), durations.end(), 0.0) / durations.size(),
                     percentile(durations, 0.5), percentile(durations, 0.95), percentile(durations, 1.0),
                     firstFailure.empty() ? "-" : firstFailure));
}

void runRegistryFallback()
{
    int ok = 0;
    std::vector<double> durations;
    for (int i = 0; i < 20; ++i)
    {
        const DWORD light = i % 2;
        const auto start = std::chrono::steady_clock::now();
        RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", REG_DWORD, &light,
                        sizeof(light));
        RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", REG_DWORD, &light,
                        sizeof(light));
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 2000, &ignored);
        durations.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        if (regDword(kPersonalize, L"SystemUsesLightTheme") == light)
        {
            ++ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    item("registry-fallback:toggles", ok == 20,
         std::format("{}/20 registry writes, broadcast p95={:.0f}ms (taskbar refresh not verifiable here)",
                     ok, percentile(durations, 0.95)));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    int toggles = 200;
    bool mitigations = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring_view arg = argv[i];
        if (arg == L"mitigations")
        {
            mitigations = true;
        }
        else
        {
            toggles = std::max(1, _wtoi(argv[i]));
        }
    }

    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));

    const auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    if (mitigations)
    {
        applyMitigations();
    }
    runThemeManager(toggles);
    if (!mitigations)
    {
        runRegistryFallback();
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
