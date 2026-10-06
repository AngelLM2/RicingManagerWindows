#include "Animator.hpp"

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace rmw::tilerapp
{
namespace
{

constexpr std::int64_t kMinFrame100ns = 41'667;  // 240 Hz
constexpr std::int64_t kMaxFrame100ns = 166'667; // 60 Hz

// One refresh of the screen, in 100 ns units (what DWM composes at); 60 Hz when it does not say.
std::int64_t refreshPeriod() noexcept
{
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    LARGE_INTEGER frequency{};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0 &&
        QueryPerformanceFrequency(&frequency) != FALSE && frequency.QuadPart > 0)
    {
        const auto period =
            static_cast<std::int64_t>((static_cast<double>(timing.qpcRefreshPeriod) * 10'000'000.0) /
                                      static_cast<double>(frequency.QuadPart));
        return std::clamp(period, kMinFrame100ns, kMaxFrame100ns);
    }
    return kMaxFrame100ns;
}

} // namespace

Animator::Animator(void* done, std::uint32_t message)
    : m_done(done)
    , m_message(message)
    , m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    , m_frame100ns(refreshPeriod())
    , m_thread([this](const std::stop_token& stop) { run(stop); })
{
}

Animator::~Animator()
{
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    if (m_timer != nullptr)
    {
        CloseHandle(m_timer);
    }
}

void Animator::start(std::vector<Move> moves, std::chrono::milliseconds duration)
{
    if (moves.empty())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        const std::scoped_lock lock(m_mutex);
        for (auto& move : moves)
        {
            const tiling::WindowId id = idOf(move.window);
            std::erase_if(m_running, [&](const Running& running) { return idOf(running.move.window) == id; });
            const std::int32_t width = move.from.width();
            const std::int32_t height = move.from.height();
            m_running.push_back({std::move(move), now, duration, width, height});
        }
    }
    m_wake.notify_all();
}

void Animator::cancel(tiling::WindowId window)
{
    const std::scoped_lock lock(m_mutex);
    std::erase_if(m_running, [&](const Running& running) { return idOf(running.move.window) == window; });
}

void Animator::cancelAll()
{
    const std::scoped_lock lock(m_mutex);
    m_running.clear();
}

bool Animator::moving(tiling::WindowId window) const
{
    const std::scoped_lock lock(m_mutex);
    return std::ranges::any_of(m_running,
                               [&](const Running& running) { return idOf(running.move.window) == window; });
}

std::optional<tiling::Rect> Animator::destination(tiling::WindowId window) const
{
    const std::scoped_lock lock(m_mutex);
    const auto found =
        std::ranges::find_if(m_running, [&](const Running& running) { return idOf(running.move.window) == window; });
    return found != m_running.end() ? std::optional(found->move.to) : std::nullopt;
}

void Animator::run(const std::stop_token& stop)
{
    struct Step
    {
        HWND window = nullptr;
        tiling::Rect frame;
        Insets insets;
        bool size = false;
    };
    std::vector<Step> steps;
    std::unique_lock lock(m_mutex);
    while (!stop.stop_requested())
    {
        if (!m_wake.wait(lock, stop, [this] { return !m_running.empty(); }))
        {
            break; // asked to stop
        }
        const auto now = std::chrono::steady_clock::now();
        steps.clear();
        bool ended = false;
        for (auto it = m_running.begin(); it != m_running.end();)
        {
            const double t =
                it->duration.count() <= 0
                    ? 1.0
                    : std::chrono::duration<double, std::milli>(now - it->start).count() / it->duration.count();
            const tiling::Rect frame = tiling::slideFrame(it->move.from, it->move.to, t);
            const bool resize = frame.width() != it->width || frame.height() != it->height;
            it->width = frame.width();
            it->height = frame.height();
            steps.push_back({static_cast<HWND>(it->move.window), frame, it->move.insets, resize});
            if (t >= 1.0)
            {
                it = m_running.erase(it);
                ended = true;
            }
            else
            {
                ++it;
            }
        }
        const bool idle = m_running.empty();
        lock.unlock();
        for (const Step& step : steps)
        {
            const tiling::Rect& f = step.frame;
            const int left = f.left - step.insets.left;
            const int top = f.top - step.insets.top;
            SetWindowPos(step.window, nullptr, left, top, f.right + step.insets.right - left,
                         f.bottom + step.insets.bottom - top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS |
                             (step.size ? 0U : static_cast<UINT>(SWP_NOSIZE)));
        }
        if (ended && idle && m_done != nullptr)
        {
            PostMessageW(static_cast<HWND>(m_done), m_message, 0, 0);
        }
        waitFrame();
        lock.lock();
    }
}

void Animator::waitFrame()
{
    if (m_timer != nullptr)
    {
        LARGE_INTEGER due{};
        due.QuadPart = -m_frame100ns; // relative
        if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) != FALSE)
        {
            WaitForSingleObject(m_timer, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(m_frame100ns / 10'000));
}

} // namespace rmw::tilerapp
