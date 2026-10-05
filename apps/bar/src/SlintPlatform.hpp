#pragma once

#include <slint-platform.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rmw::barapp
{

// Slint on our own Win32 message loop. The bar hosts its windows itself (ADR-0008 decision 5: app
// bar, position, DPI and click-through are ours); Slint only lays out and draws. One thread.
class Win32Platform final : public slint::platform::Platform
{
public:
    using AdapterFactory = std::function<std::unique_ptr<slint::platform::WindowAdapter>()>;

    explicit Win32Platform(AdapterFactory factory);
    ~Win32Platform() override;
    Win32Platform(const Win32Platform&) = delete;
    Win32Platform& operator=(const Win32Platform&) = delete;
    Win32Platform(Win32Platform&&) = delete;
    Win32Platform& operator=(Win32Platform&&) = delete;

    // The loop of this process (there is one), for widgets that wait on handles.
    [[nodiscard]] static Win32Platform* current() noexcept { return s_current; }

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override;
    void run_event_loop() override;
    void quit_event_loop() override;            // any thread
    void run_in_event_loop(Task task) override; // any thread

    // UI thread. `wake` runs on the loop when `handle` is signaled (a process, a change
    // notification). The handle stays the caller's.
    void watch(void* handle, std::function<void()> wake);
    void unwatch(void* handle);
    // Runs on every turn of the loop and at least every `period` (the worker heartbeat: a hung
    // thread stops beating).
    void everyTurn(std::function<void()> turn, std::chrono::milliseconds period);

private:
    struct Access;

    void runTasks();

    static inline Win32Platform* s_current = nullptr;

    AdapterFactory m_factory;
    void* m_messages = nullptr; // HWND, message-only: tasks and quit survive modal loops
    std::atomic<bool> m_running{false};
    std::mutex m_tasksMutex;
    std::deque<Task> m_tasks;
    struct Watch
    {
        void* handle = nullptr;
        std::function<void()> wake;
    };
    std::vector<Watch> m_watches;
    std::function<void()> m_turn;
    std::chrono::milliseconds m_period{500};
};

} // namespace rmw::barapp
