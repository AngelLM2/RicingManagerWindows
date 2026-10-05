#pragma once

#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/json/Json.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rmw::pluginhost
{

// Work handed to the host's main thread from other threads. Shared, so a thread that finishes
// after the host is gone posts into a box nobody reads instead of into freed memory.
class Inbox
{
public:
    Inbox();
    ~Inbox();
    Inbox(const Inbox&) = delete;
    Inbox& operator=(const Inbox&) = delete;
    Inbox(Inbox&&) = delete;
    Inbox& operator=(Inbox&&) = delete;

    void post(std::function<void()> task);
    [[nodiscard]] std::deque<std::function<void()>> take();
    [[nodiscard]] void* event() const noexcept { return m_event; } // signaled when something waits

private:
    std::mutex m_mutex;
    std::deque<std::function<void()>> m_tasks;
    void* m_event = nullptr; // HANDLE
};

// The pipe the core calls us on ("plugin.action", "status", "reload", "stop"). Only rmw-core.exe
// from our own folder is answered; each request is handled on the main thread (`handle` runs
// there through the inbox).
class ControlServer
{
public:
    using Handler = std::function<Result<json::Json>(const ipc::Request&)>;

    ControlServer(std::wstring pipe, std::filesystem::path coreImage, std::shared_ptr<Inbox> inbox,
                  Handler handle);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

private:
    std::jthread m_thread;
};

// Calls on the core (actions a plugin asked for, reports). Its own thread: an apply may take
// seconds, and the host's loop must not wait for it.
class CoreLink
{
public:
    explicit CoreLink(std::filesystem::path coreImage);
    ~CoreLink();
    CoreLink(const CoreLink&) = delete;
    CoreLink& operator=(const CoreLink&) = delete;
    CoreLink(CoreLink&&) = delete;
    CoreLink& operator=(CoreLink&&) = delete;

    // Queued; the answer is only logged.
    void call(std::string method, json::Json params);

private:
    struct State;
    std::shared_ptr<State> m_state;
    std::jthread m_thread;
};

} // namespace rmw::pluginhost
