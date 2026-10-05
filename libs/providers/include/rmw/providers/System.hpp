#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Data providers (docs/12 §1): what the system is doing, read the same way for the bar's own
// widgets and for plugins (dogfooding). Cheap calls, safe on any thread; the slow ones (audio,
// media) live in Volume.hpp and Media.hpp with threads of their own.
namespace rmw::providers
{

// CPU use since the previous sample (the first one only primes it).
class CpuSampler
{
public:
    [[nodiscard]] std::optional<double> sample(); // 0..100

private:
    std::uint64_t m_idle = 0;
    std::uint64_t m_busy = 0;
};

struct MemoryInfo
{
    std::uint32_t load = 0; // percent in use
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
};

[[nodiscard]] std::optional<MemoryInfo> sampleMemory();

struct NetworkRates
{
    double downBytesPerSecond = 0.0;
    double upBytesPerSecond = 0.0;
};

// Traffic of the real adapters that are up (no loopback, no virtual-switch duplicates) since the
// previous sample (the first one only primes it).
class NetworkSampler
{
public:
    [[nodiscard]] std::optional<NetworkRates> sample();

private:
    bool m_seen = false;
    std::uint64_t m_in = 0;
    std::uint64_t m_out = 0;
    std::chrono::steady_clock::time_point m_at;
};

struct BatteryInfo
{
    bool present = false; // a desktop has none
    std::uint32_t percent = 0;
    bool pluggedIn = false;
    bool charging = false;
};

[[nodiscard]] BatteryInfo sampleBattery();

// Explorer keeps the virtual desktops in the registry (there is no public API).
struct VirtualDesktops
{
    int count = 1;
    int current = 0;
};

[[nodiscard]] VirtualDesktops readVirtualDesktops();

struct MonitorInfo
{
    std::string name; // "\\.\DISPLAY1"
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::uint32_t dpi = 96;
    bool primary = false;
};

[[nodiscard]] std::vector<MonitorInfo> listMonitors();

} // namespace rmw::providers
