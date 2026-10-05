#pragma once

#include "rmw/base/Error.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Worker supervision (docs/03 §2.4): each worker runs in a job with a memory ceiling, reports a
// heartbeat, and is restarted with back-off when it crashes, leaks or hangs — until it fails too
// often, then it stays off and the owner applies its fallback.
namespace rmw::platform
{

struct WorkerSpec
{
    std::string name;
    std::filesystem::path executable;
    std::wstring arguments; // the supervisor appends --heartbeat <handle>
    // Hard ceiling on committed memory; the worker is ended at 80% of it. 0 = none. Off under
    // ASan, which reserves terabytes of shadow memory.
    std::uint64_t memoryLimitBytes = 0;
    // Heartbeat silence that counts as hung (0 = not watched), and how long a new worker may
    // take before its first beat.
    std::chrono::milliseconds hangAfter{1500};
    std::chrono::milliseconds startGrace{5000};
    // Restarts back off firstBackoff, x2, x4...; maxFailures within failureWindow = give up.
    std::chrono::milliseconds firstBackoff{1000};
    int maxFailures = 3;
    std::chrono::milliseconds failureWindow{std::chrono::minutes(5)};
    // false: the worker survives a crash of the core long enough to clean up — it gets
    // --core <handle> (the core's process) and must exit when that handle is signaled.
    bool endsWithCore = true;
    // More handles the worker inherits (inheritable; their values go in `arguments`).
    std::vector<void*> inheritHandles;
    // Job limits for workers that run code we did not write (ChildOptions).
    bool noChildProcesses = false;
    bool restrictUi = false;
};

enum class WorkerEventKind
{
    Started,
    Failed,   // see `failure`; restarted after a back-off unless Disabled follows
    Exited,   // ended by itself with code 0: not restarted
    Disabled, // failed too often: not restarted again
};

enum class WorkerFailure
{
    None,
    Crashed,     // ended with a non-zero code
    MemoryLimit, // passed 80% of its ceiling
    Hung,        // no heartbeat
    StartFailed, // could not even start
};

struct WorkerEvent
{
    std::string name;
    WorkerEventKind kind = WorkerEventKind::Started;
    WorkerFailure failure = WorkerFailure::None;
    std::uint32_t pid = 0;
    std::uint32_t exitCode = 0;
};

struct WorkerStatus
{
    std::string name;
    bool running = false;
    bool disabled = false;
    std::uint32_t pid = 0;
    int restarts = 0;
    WorkerFailure lastFailure = WorkerFailure::None;
};

// The restart decision, without processes (unit-tested on its own).
class RestartBudget
{
public:
    RestartBudget(int maxFailures, std::chrono::milliseconds window,
                  std::chrono::milliseconds firstBackoff) noexcept
        : m_maxFailures(maxFailures)
        , m_window(window)
        , m_firstBackoff(firstBackoff)
    {
    }

    // A failure at `now`: how long to wait before starting again, or nullopt to give up.
    [[nodiscard]] std::optional<std::chrono::milliseconds> onFailure(
        std::chrono::steady_clock::time_point now);

private:
    int m_maxFailures;
    std::chrono::milliseconds m_window;
    std::chrono::milliseconds m_firstBackoff;
    std::deque<std::chrono::steady_clock::time_point> m_failures;
};

class Supervisor
{
public:
    // Called on the supervisor's thread; must not call back into the supervisor.
    using Listener = std::function<void(const WorkerEvent&)>;

    [[nodiscard]] static Result<std::unique_ptr<Supervisor>> create(Listener listener);
    ~Supervisor(); // ends every worker
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&) = delete;
    Supervisor& operator=(Supervisor&&) = delete;

    // Starts a worker now (replacing one with the same name).
    [[nodiscard]] Result<void> start(WorkerSpec spec);
    // Ends a worker for good (no restart).
    void stop(std::string_view name);
    [[nodiscard]] std::vector<WorkerStatus> status() const;

private:
    struct State;
    Supervisor() = default;
    std::unique_ptr<State> m_state;
};

// The worker's side: the shared counter named by --heartbeat <handle>. Every window thread beats
// at least every 500 ms (a timer) and on every turn of its message loop.
class Heartbeat
{
public:
    // Finds --heartbeat in the arguments; a worker started by hand has none (beat() does nothing).
    [[nodiscard]] static Result<Heartbeat> fromArguments(std::span<const std::wstring_view> arguments);

    Heartbeat() noexcept = default;
    ~Heartbeat();
    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;
    Heartbeat(Heartbeat&& other) noexcept;
    Heartbeat& operator=(Heartbeat&& other) noexcept;

    void beat() noexcept;

private:
    void* m_mapping = nullptr;                  // HANDLE
    volatile std::int64_t* m_counter = nullptr; // the mapped view
};

} // namespace rmw::platform
