#include "rmw/video/Playback.hpp"

#include <algorithm>
#include <cmath>

namespace rmw::video
{

PauseReason pauseReason(const Policy& policy, const SessionState& session,
                        const MonitorState& monitor) noexcept
{
    if (session.suspending)
    {
        return PauseReason::Suspending;
    }
    if (session.displayOff)
    {
        return PauseReason::DisplayOff;
    }
    if (session.locked)
    {
        return PauseReason::Locked;
    }
    if (policy.pauseOnFullscreen && monitor.fullscreen)
    {
        return PauseReason::Fullscreen;
    }
    if (policy.pauseOnBattery && (session.onBattery || session.batterySaver))
    {
        return PauseReason::Battery;
    }
    if (policy.pauseWhenCovered && monitor.covered)
    {
        return PauseReason::Covered;
    }
    return PauseReason::None;
}

std::string_view toString(PauseReason reason) noexcept
{
    switch (reason)
    {
    case PauseReason::None:
        return "playing";
    case PauseReason::Suspending:
        return "the PC is going to sleep";
    case PauseReason::DisplayOff:
        return "the display is off";
    case PauseReason::Locked:
        return "the session is locked";
    case PauseReason::Fullscreen:
        return "a fullscreen app is in front";
    case PauseReason::Battery:
        return "on battery";
    case PauseReason::Covered:
        return "the desktop is covered";
    }
    return "?";
}

Placement place(domain::WallpaperFit fit, int videoWidth, int videoHeight, int surfaceWidth,
                int surfaceHeight) noexcept
{
    const Rect wholeVideo{0, 0, videoWidth, videoHeight};
    const Rect wholeSurface{0, 0, surfaceWidth, surfaceHeight};
    if (videoWidth <= 0 || videoHeight <= 0 || surfaceWidth <= 0 || surfaceHeight <= 0)
    {
        return {wholeVideo, wholeSurface};
    }
    const double vw = videoWidth;
    const double vh = videoHeight;
    const double sw = surfaceWidth;
    const double sh = surfaceHeight;
    // A rectangle of `w` x `h` centered in `outer`.
    const auto centered = [](int outerWidth, int outerHeight, int w, int h) {
        const int left = (outerWidth - w) / 2;
        const int top = (outerHeight - h) / 2;
        return Rect{left, top, left + w, top + h};
    };
    switch (fit)
    {
    case domain::WallpaperFit::Stretch:
    case domain::WallpaperFit::Tile: // the parser refuses tile for videos; stretch if it ever comes
        return {wholeVideo, wholeSurface};
    case domain::WallpaperFit::Fill: {
        // Scale to cover the surface; the video's middle part is what shows.
        const double scale = std::max(sw / vw, sh / vh);
        const int shownWidth = std::clamp(static_cast<int>(std::lround(sw / scale)), 1, videoWidth);
        const int shownHeight = std::clamp(static_cast<int>(std::lround(sh / scale)), 1, videoHeight);
        return {centered(videoWidth, videoHeight, shownWidth, shownHeight), wholeSurface};
    }
    case domain::WallpaperFit::Fit: {
        // Scale to show all of it; bars where the aspect ratios differ.
        const double scale = std::min(sw / vw, sh / vh);
        const int width = std::clamp(static_cast<int>(std::lround(vw * scale)), 1, surfaceWidth);
        const int height = std::clamp(static_cast<int>(std::lround(vh * scale)), 1, surfaceHeight);
        return {wholeVideo, centered(surfaceWidth, surfaceHeight, width, height)};
    }
    case domain::WallpaperFit::Center: {
        // Pixel for pixel: the overlapping middle parts of both.
        const int width = std::min(videoWidth, surfaceWidth);
        const int height = std::min(videoHeight, surfaceHeight);
        return {centered(videoWidth, videoHeight, width, height),
                centered(surfaceWidth, surfaceHeight, width, height)};
    }
    }
    return {wholeVideo, wholeSurface};
}

FramePacer::FramePacer(std::uint32_t fps) noexcept
    : m_period(fps == 0
                   ? Clock::duration::zero()
                   : std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps)))
{
}

bool FramePacer::due(Clock::time_point now) noexcept
{
    if (m_period == Clock::duration::zero())
    {
        return true;
    }
    if (now < m_next)
    {
        return false;
    }
    // Late by more than a period (a pause, a hiccup): start counting from now, no burst.
    m_next = now - m_next > m_period ? now + m_period : m_next + m_period;
    return true;
}

} // namespace rmw::video
