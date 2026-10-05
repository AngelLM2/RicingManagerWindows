#include "rmw/providers/System.hpp"

#include "rmw/base/Utf.hpp"

// The network tables need Winsock's headers before <windows.h>: keep this order.
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
// clang-format on

#include <wil/resource.h>

#include <shellscalingapi.h>

#include <algorithm>
#include <span>

namespace rmw::providers
{
namespace
{

std::uint64_t ticks(const FILETIME& time) noexcept
{
    return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}

std::vector<GUID> readIds(HKEY key, const wchar_t* value)
{
    std::vector<GUID> ids;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, value, nullptr, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes % sizeof(GUID) != 0 || bytes > 64 * sizeof(GUID))
    {
        return ids;
    }
    ids.resize(bytes / sizeof(GUID));
    if (RegQueryValueExW(key, value, nullptr, nullptr, reinterpret_cast<BYTE*>(ids.data()), &bytes) !=
        ERROR_SUCCESS)
    {
        ids.clear();
    }
    return ids;
}

} // namespace

std::optional<double> CpuSampler::sample()
{
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == FALSE)
    {
        return std::nullopt;
    }
    const std::uint64_t idleNow = ticks(idle);
    const std::uint64_t busyNow = ticks(kernel) + ticks(user); // kernel time includes idle
    std::optional<double> load;
    if (m_busy != 0 && busyNow > m_busy)
    {
        const auto total = static_cast<double>(busyNow - m_busy);
        load = std::clamp(1.0 - (static_cast<double>(idleNow - m_idle) / total), 0.0, 1.0) * 100.0;
    }
    m_idle = idleNow;
    m_busy = busyNow;
    return load;
}

std::optional<MemoryInfo> sampleMemory()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == FALSE)
    {
        return std::nullopt;
    }
    return MemoryInfo{status.dwMemoryLoad, status.ullTotalPhys, status.ullAvailPhys};
}

std::optional<NetworkRates> NetworkSampler::sample()
{
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
    {
        return std::nullopt;
    }
    std::uint64_t in = 0;
    std::uint64_t out = 0;
    const std::span rows(static_cast<const MIB_IF_ROW2*>(table->Table), table->NumEntries);
    for (const auto& row : rows)
    {
        // Real adapters that are up: no loopback, no filter and virtual-switch duplicates.
        if (row.OperStatus == IfOperStatusUp && row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE &&
            row.Type != IF_TYPE_SOFTWARE_LOOPBACK)
        {
            in += row.InOctets;
            out += row.OutOctets;
        }
    }
    FreeMibTable(table);
    const auto now = std::chrono::steady_clock::now();
    std::optional<NetworkRates> rates;
    if (m_seen)
    {
        const double seconds = std::chrono::duration<double>(now - m_at).count();
        if (seconds > 0.1 && in >= m_in && out >= m_out)
        {
            rates = NetworkRates{static_cast<double>(in - m_in) / seconds,
                                 static_cast<double>(out - m_out) / seconds};
        }
    }
    m_seen = true;
    m_in = in;
    m_out = out;
    m_at = now;
    return rates;
}

BatteryInfo sampleBattery()
{
    SYSTEM_POWER_STATUS power{};
    constexpr BYTE kNoBattery = 128;
    constexpr BYTE kUnknown = 255;
    constexpr BYTE kCharging = 8;
    BatteryInfo info;
    if (GetSystemPowerStatus(&power) == FALSE)
    {
        return info;
    }
    info.pluggedIn = power.ACLineStatus == 1;
    if (power.BatteryFlag == kNoBattery || power.BatteryFlag == kUnknown || power.BatteryLifePercent > 100)
    {
        return info;
    }
    info.present = true;
    info.percent = power.BatteryLifePercent;
    info.charging = (power.BatteryFlag & kCharging) != 0;
    return info;
}

VirtualDesktops readVirtualDesktops()
{
    VirtualDesktops desktops;
    wil::unique_hkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops)", 0, KEY_READ,
                      key.put()) != ERROR_SUCCESS)
    {
        return desktops;
    }
    auto ids = readIds(key.get(), L"VirtualDesktopIDs");
    const auto current = readIds(key.get(), L"CurrentVirtualDesktop");
    if (ids.empty())
    {
        ids.resize(1); // never switched desktops yet: there is one
    }
    desktops.count = static_cast<int>(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        if (!current.empty() && IsEqualGUID(ids[i], current.front()) != FALSE)
        {
            desktops.current = static_cast<int>(i);
        }
    }
    return desktops;
}

std::vector<MonitorInfo> listMonitors()
{
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto& list =
                *reinterpret_cast<std::vector<MonitorInfo>*>(data); // NOLINT(performance-no-int-to-ptr)
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) == FALSE)
            {
                return TRUE;
            }
            MonitorInfo entry;
            entry.name = utf16To8Lossy(info.szDevice);
            entry.x = info.rcMonitor.left;
            entry.y = info.rcMonitor.top;
            entry.width = info.rcMonitor.right - info.rcMonitor.left;
            entry.height = info.rcMonitor.bottom - info.rcMonitor.top;
            entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            {
                entry.dpi = dpiX;
            }
            list.push_back(std::move(entry));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

} // namespace rmw::providers
