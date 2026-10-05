#pragma once

#include "rmw/domain/Rice.hpp"
#include "rmw/video/Plan.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

// The decisions of rmw-wallpaper that need no Windows (docs/04 M6): when to pause, where the frame
// goes, when the next one is due.
namespace rmw::video
{

using Clock = std::chrono::steady_clock;

// Paused this long, a monitor's decoder and swap chain are freed: the window hides and Windows
// shows the rice's fallback image underneath; playing again builds them anew.
inline constexpr std::chrono::seconds kReleaseAfter{120};

// What the whole session is like.
struct SessionState
{
    bool onBattery = false;    // the power cord is out
    bool batterySaver = false; // Windows' energy saver is on
    bool locked = false;
    bool displayOff = false;
    bool suspending = false;
};

// What one monitor is like.
struct MonitorState
{
    bool fullscreen = false; // a fullscreen app (a game, a video) is in front there
    bool covered = false;    // a maximized window hides the desktop there
};

enum class PauseReason : std::uint8_t
{
    None,
    Suspending,
    DisplayOff,
    Locked,
    Fullscreen,
    Battery,
    Covered,
};

// Why a monitor's video is paused (the first reason that applies, most important first).
[[nodiscard]] PauseReason pauseReason(const Policy& policy, const SessionState& session,
                                      const MonitorState& monitor) noexcept;
[[nodiscard]] std::string_view toString(PauseReason reason) noexcept;

struct Rect
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    friend bool operator==(const Rect&, const Rect&) = default;
};

// Which part of the video goes where in the monitor's surface; what lies outside `target` is black.
struct Placement
{
    Rect source; // in video pixels
    Rect target; // in surface pixels
    friend bool operator==(const Placement&, const Placement&) = default;
};

// Fill covers the surface (cropping the video), fit shows all of it (bars), stretch ignores the
// aspect ratio, center keeps the video's size (cropped when larger).
[[nodiscard]] Placement place(domain::WallpaperFit fit, int videoWidth, int videoHeight, int surfaceWidth,
                              int surfaceHeight) noexcept;

// Spaces the frames out to at most `fps` a second (0: no limit). Frames are never shown early.
class FramePacer
{
public:
    explicit FramePacer(std::uint32_t fps) noexcept;
    // Whether a frame may be shown now; when it may, the next one is due a period later.
    [[nodiscard]] bool due(Clock::time_point now) noexcept;
    void reset() noexcept { m_next = {}; }

private:
    Clock::duration m_period{};
    Clock::time_point m_next;
};

} // namespace rmw::video
