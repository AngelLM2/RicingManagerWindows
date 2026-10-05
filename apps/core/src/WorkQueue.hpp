#pragma once

#include "rmw/base/Log.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace rmw::core
{

// One background thread running tasks in order: the resident work (schedule, rules, hotkeys,
// drift, reapply) never blocks the window's message loop (docs/03 §9).
class WorkQueue
{
public:
    WorkQueue()
        : m_thread([this](const std::stop_token& stop) { loop(stop); })
    {
    }

    ~WorkQueue() { stop(); }

    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;
    WorkQueue(WorkQueue&&) = delete;
    WorkQueue& operator=(WorkQueue&&) = delete;

    void push(std::function<void()> task)
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_wake.notify_one();
    }

    // Drops what has not started, waits for the running task. Idempotent.
    void stop()
    {
        m_thread.request_stop();
        m_wake.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    [[nodiscard]] bool idle()
    {
        const std::scoped_lock lock(m_mutex);
        return m_tasks.empty() && !m_running;
    }

private:
    void loop(const std::stop_token& stop)
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, stop, [this] { return !m_tasks.empty(); });
                if (stop.stop_requested())
                {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
                m_running = true;
            }
            // A failing task must not take the thread (and the process) with it.
            try
            {
                task();
            }
            catch (const std::exception& e)
            {
                log::error("background task failed: {}", e.what());
            }
            catch (...)
            {
                log::error("background task failed: unknown exception");
            }
            const std::scoped_lock lock(m_mutex);
            m_running = false;
        }
    }

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::function<void()>> m_tasks;
    bool m_running = false;
    std::jthread m_thread; // last: starts after the members it uses exist
};

} // namespace rmw::core
