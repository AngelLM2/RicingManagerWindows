#include "rmw/platform/Customizers.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace rmw::platform
{
namespace
{

struct Known
{
    std::wstring_view process; // executable name, compared without case
    std::string_view product;
};

constexpr std::array<Known, 16> kProcesses = {{
    {L"wallpaper32.exe", "Wallpaper Engine"},
    {L"wallpaper64.exe", "Wallpaper Engine"},
    {L"Lively.exe", "Lively Wallpaper"},
    {L"Lively.UI.WinUI.exe", "Lively Wallpaper"},
    {L"AutoDarkModeSvc.exe", "Auto Dark Mode"},
    {L"TranslucentTB.exe", "TranslucentTB"},
    {L"windhawk.exe", "Windhawk"},
    {L"Rainmeter.exe", "Rainmeter"},
    {L"Start11Srv.exe", "Start11"},
    {L"WindowBlinds.exe", "WindowBlinds"},
    {L"DisplayFusion.exe", "DisplayFusion"},
    {L"yasb.exe", "YASB"},
    {L"seelen-ui.exe", "Seelen UI"},
    {L"zebar.exe", "Zebar"},
    {L"RoundedTB.exe", "RoundedTB"},
    {L"TaskbarX.exe", "TaskbarX"},
}};

// Tools that live inside Explorer (no process of their own) leave a key behind.
struct KnownKey
{
    const wchar_t* key; // NUL-terminated for the registry API
    std::string_view product;
};

constexpr std::array<KnownKey, 2> kKeys = {{
    {LR"(Software\ExplorerPatcher)", "ExplorerPatcher"},
    {LR"(Software\StartIsBack)", "StartAllBack"},
}};

bool sameName(std::wstring_view a, std::wstring_view b) noexcept
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

} // namespace

std::vector<std::string> detectOtherCustomizers()
{
    std::vector<std::string> found;
    const wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE;
             more = Process32NextW(snapshot.get(), &entry))
        {
            for (const auto& known : kProcesses)
            {
                if (sameName(entry.szExeFile, known.process))
                {
                    found.emplace_back(known.product);
                }
            }
        }
    }
    for (const auto& known : kKeys)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, known.key, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            RegCloseKey(key);
            found.emplace_back(known.product);
        }
    }
    std::ranges::sort(found);
    const auto duplicates = std::ranges::unique(found);
    found.erase(duplicates.begin(), duplicates.end());
    return found;
}

} // namespace rmw::platform
