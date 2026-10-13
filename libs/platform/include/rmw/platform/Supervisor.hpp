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
    // For workers that run code we did not write: no child processes (ChildOptions).
    bool noChildProcesses = false;
    // For a worker that opens the person's apps (the bar): they are not in its job (ChildOptions).
    bool childrenLeaveJob = false;
};

enum class WorkerEventKind
{
    Started,
    Failed,   // see `failure`; restarted after a back-off unless Disabled follows
    Exited,   // ended by itself with code 0: not restarted
    Disabled, // failed too often: not restarted again
    // Asked to leave (Supervisor::leave), it is gone: `failure` None when it left by itself with code 0,
    // Hung when it had to be ended, Crashed when it ended with another code. Never restarted.
    Left,
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

// The hang decision for one worker, without processes (unit-tested on its own). Time the supervisor
// itself did not run (the PC slept: its clock counts on, the worker could not beat) is not silence:
// a gap between two looks longer than the silence allowed, or a resume, starts a new grace.
class HangWatch
{
public:
    using Clock = std::chrono::steady_clock;

    HangWatch(std::chrono::milliseconds hangAfter, std::chrono::milliseconds startGrace) noexcept
        : m_hangAfter(hangAfter)
        , m_startGrace(startGrace)
    {
    }

    // A new process, whose counter starts at 0.
    void start(Clock::time_point now) noexcept;
    // One look at the counter at `now`: true when the worker went silent for too long.
    [[nodiscard]] bool hung(std::int64_t beat, Clock::time_point now) noexcept;
    // The PC goes to sleep (no verdicts) and comes back (a new grace from `now`).
    void pause() noexcept { m_paused = true; }
    void resume(Clock::time_point now) noexcept;

private:
    void regrace(Clock::time_point now) noexcept;

    std::chrono::milliseconds m_hangAfter;
    std::chrono::milliseconds m_startGrace;
    std::int64_t m_lastBeat = 0;
    bool m_beaten = false;
    bool m_paused = false;
    Clock::time_point m_lastBeatAt;
    Clock::time_point m_graceFrom; // the start, or the end of a gap: the grace counts from here
    Clock::time_point m_lastLook;
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
    // Ends a worker for good (no restart), now.
    void stop(std::string_view name);
    // Ends a worker for good, letting it leave by itself first: from now on it is leaving (no restart,
    // no hang checks), `ask` asks it with its pid (a message to its window), and one still there after
    // `patience` is ended. Returns at once; a Left event says how it went. False when it was not running.
    bool leave(std::string_view name, const std::function<void(std::uint32_t pid)>& ask,
               std::chrono::milliseconds patience);
    // Until no worker is leaving (or `timeout`): true when none is.
    [[nodiscard]] bool waitLeft(std::chrono::milliseconds timeout);
    // The PC goes to sleep (true) or is back (false): no worker is taken for hung meanwhile, and each
    // gets a new grace after it.
    void setSuspended(bool suspended);
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
