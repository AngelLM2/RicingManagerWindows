#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// What rmw-tiler tells the bars and how a bar talks back (M13): a block of shared memory with each
// monitor's workspaces, an event set when it changes, and a window that takes "go to workspace N".
// Everything per session ("Local\").
namespace rmw::tiling
{

inline constexpr const wchar_t* kStateMappingName = L"Local\\RicingManagerWindows.Tiler.State";
inline constexpr const wchar_t* kStateEventName = L"Local\\RicingManagerWindows.Tiler.Changed";
inline constexpr const wchar_t* kControlClass = L"RicingManagerWindows.TilerControl";

// To the control window: wParam = the workspace (1..9), lParam = a point on the monitor (x and y as
// signed 16-bit numbers, as GET_X_LPARAM reads them).
inline constexpr std::uint32_t kGoToMessage = 0x8000 + 0x51; // WM_APP + 0x51

inline constexpr std::uint32_t kSharedVersion = 1;
inline constexpr std::size_t kMaxSharedMonitors = 16;

struct SharedMonitor
{
    std::int32_t left = 0; // the whole monitor
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::uint32_t occupied = 0; // bit i: workspace i has windows
    std::uint8_t active = 0;    // from 0
    std::uint8_t layout = 0;    // domain::TilingLayout of the active workspace
    std::array<std::uint8_t, 2> reserved{};
};

// The writer makes `sequence` odd, writes, and makes it even again; a reader copies the block and
// keeps the copy only when `sequence` was the same even number before and after.
struct SharedState
{
    std::uint32_t version = kSharedVersion;
    std::uint32_t sequence = 0;
    std::uint32_t workspaces = 0; // on each monitor
    std::uint32_t monitorCount = 0;
    std::uint8_t mode = 0;   // domain::TilingMode
    std::uint8_t paused = 0; // tiling is off for now (Toggle)
    std::array<std::uint8_t, 2> reserved{};
    std::array<SharedMonitor, kMaxSharedMonitors> monitors{};
};

static_assert(std::is_trivially_copyable_v<SharedState>);

} // namespace rmw::tiling
