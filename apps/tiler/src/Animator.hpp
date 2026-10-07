#pragma once

#include "WindowOps.hpp"

#include "rmw/tiling/Layout.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rmw::tilerapp
{

// Windows sliding to their places (v1.8): a workspace that comes in, a window that opens, two that trade
// places. Only the position moves, frame by frame, on a thread of its own in step with the screen; the size
// changes at most twice, at the start and at the end (an app repaints on every resize, and Chromium-based
// ones flash while they do). Every move is asynchronous (SWP_ASYNCWINDOWPOS): a hung app never holds the
// others up. The tiler's thread hands moves in and is told when they are all over.
class Animator
{
public:
    struct Move
    {
        Window window = nullptr;
        tiling::Rect from; // the visible frame where it starts
        tiling::Rect to;   // where it ends
        Insets insets;     // the invisible borders around the frame
    };

    // `done` (an HWND): told `message` when the last move has ended.
    Animator(void* done, std::uint32_t message);
    ~Animator(); // stops at once: windows stay where the last frame put them
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;
    Animator(Animator&&) = delete;
    Animator& operator=(Animator&&) = delete;

    // A window already moving starts again from where it is now.
    void start(const std::vector<Move>& moves, std::chrono::milliseconds duration);
    void cancel(tiling::WindowId window); // a drag, a window that goes away
    void cancelAll();
    [[nodiscard]] bool moving(tiling::WindowId window) const;
    // Where a moving window ends (its slot), or nothing.
    [[nodiscard]] std::optional<tiling::Rect> destination(tiling::WindowId window) const;

private:
    struct Running
    {
        Move move;
        std::chrono::steady_clock::time_point start;
        std::chrono::milliseconds duration{};
        std::int32_t width = 0; // the size it has now (a frame sends one only when it changes)
        std::int32_t height = 0;
    };

    void run(const std::stop_token& stop);
    void waitFrame();

    void* m_done;
    std::uint32_t m_message;
    void* m_timer = nullptr; // a high-resolution waitable timer: one screen refresh
    std::int64_t m_frame100ns = 166'667;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::vector<Running> m_running;
    std::jthread m_thread; // last: it stops before what it uses goes away
};

} // namespace rmw::tilerapp
