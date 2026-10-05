#pragma once

#include "rmw/bar/Tray.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rmw::barapp
{

// Keeps Explorer's tray in step with what apps tell our tray window, on a thread of its own: the
// tray window answers apps at once, and nothing that waits for Explorer (it is slow while it
// restarts) piles up in front of it — our bar's own app-bar calls go through that window too.
//
// Each call keeps its order per icon. What Explorer refuses (it does, for a while after it
// restarts) is sent again every 2 s for 2 minutes. Explorer gets our copy of the app's icon,
// made while the app was still waiting in Shell_NotifyIcon: apps destroy theirs right after.
class TrayForwarder
{
public:
    TrayForwarder();
    ~TrayForwarder(); // pending copies are dropped; apps are asked for their icons afterwards
    TrayForwarder(const TrayForwarder&) = delete;
    TrayForwarder& operator=(const TrayForwarder&) = delete;
    TrayForwarder(TrayForwarder&&) = delete;
    TrayForwarder& operator=(TrayForwarder&&) = delete;

    // A Shell_NotifyIcon call as received (decodeUpdate accepted it), from the tray thread.
    void send(std::vector<std::byte> payload, std::uintptr_t wParam, const bar::tray::Update& update);

private:
    using Clock = std::chrono::steady_clock;
    struct Job
    {
        std::vector<std::byte> payload;
        std::uintptr_t wParam = 0;
        bar::tray::Command command = bar::tray::Command::Add;
        void* icon = nullptr; // HICON: our copy, destroyed once Explorer has it
        int attempts = 0;
    };
    struct Pending
    {
        std::deque<Job> jobs;
        Clock::time_point due;
    };

    void run(const std::stop_token& stop);
    // True when the job is done (Explorer has it, or there is nothing to retry).
    bool deliver(Job& job);
    static void release(Job& job) noexcept;

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::pair<std::string, Job>> m_queue; // new calls, by icon
    std::map<std::string, Pending> m_pending;        // what Explorer refused, by icon (its thread only)
    std::jthread m_thread;                           // last: stops before the rest goes
};

} // namespace rmw::barapp
