#include "rmw/platform/Supervisor.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace rmw::platform
{
namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr ULONG_PTR kWakeKey = 0; // launches use keys from 1
constexpr auto kTick = 250ms;     // heartbeat and exit checks while workers exist

std::string_view failureName(WorkerFailure failure) noexcept
{
    switch (failure)
    {
    case WorkerFailure::Crashed:
        return "crashed";
    case WorkerFailure::MemoryLimit:
        return "passed its memory limit";
    case WorkerFailure::Hung:
        return "stopped responding";
    case WorkerFailure::StartFailed:
        return "could not start";
    case WorkerFailure::None:
        break;
    }
    return "ended";
}

struct Worker
{
    explicit Worker(WorkerSpec workerSpec)
        : spec(std::move(workerSpec))
        , budget(spec.maxFailures, spec.failureWindow, spec.firstBackoff)
        , watch(spec.hangAfter, spec.startGrace)
    {
    }

    WorkerSpec spec;
    RestartBudget budget;
    HangWatch watch;
    ChildProcess process;
    wil::unique_handle mapping;
    wil::unique_mapview_ptr<std::int64_t> heartbeat;
    std::uint64_t key = 0; // completion key of the running launch; 0 = not running
    WorkerFailure ending = WorkerFailure::None; // why we are ending it
    std::optional<Clock::time_point> restartAt;
    bool disabled = false;
    int restarts = 0;
    WorkerFailure lastFailure = WorkerFailure::None;
    // Asked to leave (Supervisor::leave): ended at `leaveBy` if still there; `gone` once it is.
    bool leaving = false;
    Clock::time_point leaveBy;
    bool gone = false;
};

} // namespace

void HangWatch::start(Clock::time_point now) noexcept
{
    m_lastBeat = 0;
    m_beaten = false;
    m_lastBeatAt = now;
    m_graceFrom = now;
    m_lastLook = now;
}

bool HangWatch::hung(std::int64_t beat, Clock::time_point now) noexcept
{
    if (m_hangAfter.count() <= 0)
    {
        return false; // not watched
    }
    // Looks come every quarter second: a longer gap is time this thread did not run (sleep,
    // hibernation), in which the worker could not beat either.
    const bool gap = now - m_lastLook > m_hangAfter;
    m_lastLook = now;
    if (beat != m_lastBeat)
    {
        m_lastBeat = beat;
        m_lastBeatAt = now;
        m_beaten = true;
    }
    if (m_paused)
    {
        return false;
    }
    if (gap)
    {
        regrace(now);
        return false;
    }
    return m_beaten ? now - m_lastBeatAt > m_hangAfter : now - m_graceFrom > m_startGrace;
}

void HangWatch::resume(Clock::time_point now) noexcept
{
    m_paused = false;
    m_lastLook = now;
    regrace(now);
}

void HangWatch::regrace(Clock::time_point now) noexcept
{
    // As after a start: back from sleep, a window thread first catches up (displays, DWM, broadcasts).
    m_beaten = false;
    m_graceFrom = now;
    m_lastBeatAt = now;
}

std::optional<std::chrono::milliseconds> RestartBudget::onFailure(Clock::time_point now)
{
    m_failures.push_back(now);
    while (!m_failures.empty() && now - m_failures.front() > m_window)
    {
        m_failures.pop_front();
    }
    const auto count = static_cast<int>(m_failures.size());
    if (count >= m_maxFailures)
    {
        return std::nullopt;
    }
    return m_firstBackoff * (std::int64_t{1} << std::min(count - 1, 6));
}

struct Supervisor::State
{
    Listener listener;
    wil::unique_handle port;
    mutable std::mutex mutex;
    std::condition_variable left; // a leaving worker is gone
    std::map<std::string, std::unique_ptr<Worker>, std::less<>> workers;
    std::vector<WorkerEvent> pending; // emitted by the supervisor thread
    std::uint64_t nextKey = 1;
    bool suspended = false;
    std::jthread thread; // last: starts after everything it uses exists

    void wake() const noexcept { PostQueuedCompletionStatus(port.get(), 0, kWakeKey, nullptr); }

    Result<void> launch(Worker& worker)
    {
        worker.ending = WorkerFailure::None;
        worker.restartAt.reset();
        // A fresh counter per launch: a beat of the old process never counts for the new one.
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        wil::unique_handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, 0,
                                                      sizeof(std::int64_t), nullptr));
        if (!mapping)
        {
            return std::unexpected(lastError("CreateFileMapping"));
        }
        wil::unique_mapview_ptr<std::int64_t> view(static_cast<std::int64_t*>(
            MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(std::int64_t))));
        if (!view)
        {
            return std::unexpected(lastError("MapViewOfFile"));
        }
        const std::uint64_t key = nextKey++;
        ChildOptions options;
        options.memoryLimitBytes = worker.spec.memoryLimitBytes;
        options.memoryNotifyBytes = worker.spec.memoryLimitBytes / 10 * 8;
        options.completionPort = port.get();
        options.completionKey = key;
        options.killOnClose = worker.spec.endsWithCore;
        options.noChildProcesses = worker.spec.noChildProcesses;
        options.childrenLeaveJob = worker.spec.childrenLeaveJob;
        options.inheritHandles = {mapping.get()};
        options.inheritHandles.insert(options.inheritHandles.end(), worker.spec.inheritHandles.begin(),
                                      worker.spec.inheritHandles.end());
        std::wstring arguments = std::format(L"{} --heartbeat {}", worker.spec.arguments,
                                             reinterpret_cast<std::uintptr_t>(mapping.get()));
        wil::unique_handle core; // our copy closes after the spawn; the child keeps its own
        if (!worker.spec.endsWithCore)
        {
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), core.put(),
                                SYNCHRONIZE, TRUE, 0) == FALSE)
            {
                return std::unexpected(lastError("DuplicateHandle(core)"));
            }
            options.inheritHandles.push_back(core.get());
            arguments += std::format(L" --core {}", reinterpret_cast<std::uintptr_t>(core.get()));
        }
        auto child = ChildProcess::spawn(worker.spec.executable, arguments, options);
        if (!child)
        {
            return std::unexpected(std::move(child.error()));
        }
        worker.process = std::move(*child);
        worker.mapping = std::move(mapping);
        worker.heartbeat = std::move(view);
        worker.key = key;
        worker.watch.start(Clock::now());
        if (suspended)
        {
            worker.watch.pause();
        }
        log::info("worker {} started (pid {})", worker.spec.name, worker.process.pid());
        pending.push_back(
            {worker.spec.name, WorkerEventKind::Started, WorkerFailure::None, worker.process.pid(), 0});
        return {};
    }

    void recordFailure(Worker& worker, WorkerFailure failure, std::uint32_t pid, std::uint32_t exitCode)
    {
        worker.lastFailure = failure;
        pending.push_back({worker.spec.name, WorkerEventKind::Failed, failure, pid, exitCode});
        if (const auto delay = worker.budget.onFailure(Clock::now()))
        {
            log::warn("worker {} {} (code 0x{:08X}); restarting in {} ms", worker.spec.name,
                      failureName(failure), exitCode, delay->count());
            worker.restartAt = Clock::now() + *delay;
            return;
        }
        log::error("worker {} {} too often: disabled", worker.spec.name, failureName(failure));
        worker.disabled = true;
        pending.push_back({worker.spec.name, WorkerEventKind::Disabled, failure, pid, exitCode});
    }

    void ended(Worker& worker)
    {
        // The job reports the exit as the process ends: give the handle a moment to agree.
        const std::uint32_t code = worker.process.wait(2s).value_or(STILL_ACTIVE);
        const std::uint32_t pid = worker.process.pid();
        worker.process = ChildProcess{};
        worker.heartbeat.reset();
        worker.mapping.reset();
        worker.key = 0;
        WorkerFailure failure = worker.ending;
        if (worker.leaving)
        {
            // Asked to go: never a failure to count, never restarted. Whoever asked hears how it went (a
            // worker ended before it could clean up leaves things for its owner to put back).
            if (failure == WorkerFailure::None && code != 0)
            {
                failure = WorkerFailure::Crashed;
            }
            if (failure == WorkerFailure::None)
            {
                log::info("worker {} left", worker.spec.name);
            }
            else
            {
                log::warn("worker {} did not leave by itself ({}, code 0x{:08X})", worker.spec.name,
                          failureName(failure), code);
            }
            pending.push_back({worker.spec.name, WorkerEventKind::Left, failure, pid, code});
            worker.gone = true;
            return;
        }
        if (failure == WorkerFailure::None)
        {
            if (code == 0)
            {
                log::info("worker {} exited", worker.spec.name);
                pending.push_back({worker.spec.name, WorkerEventKind::Exited, WorkerFailure::None, pid, 0});
                return;
            }
            failure = WorkerFailure::Crashed;
        }
        recordFailure(worker, failure, pid, code);
    }

    void end(Worker& worker, WorkerFailure why)
    {
        if (worker.ending == WorkerFailure::None)
        {
            worker.ending = why;
            worker.process.terminate();
        }
    }

    void onJobMessage(ULONG_PTR key, DWORD message)
    {
        const auto found =
            std::ranges::find_if(workers, [&](const auto& entry) { return entry.second->key == key; });
        if (found == workers.end())
        {
            return; // a launch that is already gone
        }
        Worker& worker = *found->second;
        switch (message)
        {
        case JOB_OBJECT_MSG_NOTIFICATION_LIMIT:
        case JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT:
        case JOB_OBJECT_MSG_JOB_MEMORY_LIMIT:
            end(worker, WorkerFailure::MemoryLimit);
            break;
        case JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO:
            ended(worker);
            break;
        default:
            break;
        }
    }

    void tick()
    {
        const auto now = Clock::now();
        for (auto& [name, slot] : workers)
        {
            Worker& worker = *slot;
            if (worker.key != 0)
            {
                // Job messages may be dropped; the process handle never lies (docs/03 §2.4).
                if (!worker.process.running())
                {
                    ended(worker);
                    continue;
                }
                if (worker.leaving)
                {
                    // Cleaning up, it may not beat: only the deadline counts.
                    if (worker.ending == WorkerFailure::None && now >= worker.leaveBy)
                    {
                        log::warn("worker {} did not leave in time; ending it", name);
                        end(worker, WorkerFailure::Hung);
                    }
                    continue;
                }
                if (worker.ending == WorkerFailure::None &&
                    worker.watch.hung(ReadNoFence64(worker.heartbeat.get()), now))
                {
                    end(worker, WorkerFailure::Hung);
                }
            }
            else if (worker.restartAt && now >= *worker.restartAt && !worker.disabled)
            {
                ++worker.restarts;
                if (auto started = launch(worker); !started)
                {
                    log::warn("worker {}: {}", name, started.error().toString());
                    recordFailure(worker, WorkerFailure::StartFailed, 0, 0);
                }
            }
        }
        if (std::erase_if(workers, [](const auto& entry) { return entry.second->gone; }) > 0)
        {
            left.notify_all();
        }
    }

    [[nodiscard]] bool anyLeaving() const
    {
        return std::ranges::any_of(workers, [](const auto& entry) { return entry.second->leaving; });
    }

    [[nodiscard]] DWORD nextWait() const
    {
        std::optional<Clock::duration> wait;
        const auto now = Clock::now();
        for (const auto& [name, worker] : workers)
        {
            if (worker->key != 0)
            {
                wait = std::min<Clock::duration>(wait.value_or(kTick), kTick);
            }
            else if (worker->restartAt && !worker->disabled)
            {
                const auto until =
                    std::max<Clock::duration>(*worker->restartAt - now, Clock::duration::zero());
                wait = std::min(wait.value_or(until), until);
            }
        }
        if (!wait)
        {
            return INFINITE; // nothing to watch: sleep until a job message or a call
        }
        return static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(*wait).count());
    }

    void loop(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            DWORD wait = INFINITE;
            {
                const std::scoped_lock lock(mutex);
                wait = nextWait();
            }
            DWORD message = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED overlapped = nullptr;
            const BOOL got = GetQueuedCompletionStatus(port.get(), &message, &key, &overlapped, wait);
            std::vector<WorkerEvent> events;
            {
                const std::scoped_lock lock(mutex);
                if (got != FALSE && key != kWakeKey)
                {
                    onJobMessage(key, message);
                }
                tick();
                events.swap(pending);
            }
            for (const auto& event : events)
            {
                try
                {
                    listener(event);
                }
                catch (const std::exception& e)
                {
                    log::error("worker event listener failed: {}", e.what());
                }
            }
        }
    }
};

Result<std::unique_ptr<Supervisor>> Supervisor::create(Listener listener)
{
    auto supervisor = std::unique_ptr<Supervisor>(new Supervisor());
    supervisor->m_state = std::make_unique<State>();
    State& state = *supervisor->m_state;
    state.listener = listener ? std::move(listener) : [](const WorkerEvent&) {};
    state.port.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!state.port)
    {
        return std::unexpected(lastError("CreateIoCompletionPort"));
    }
    state.thread = std::jthread([&state](const std::stop_token& stop) { state.loop(stop); });
    return supervisor;
}

Supervisor::~Supervisor()
{
    if (m_state && m_state->thread.joinable())
    {
        m_state->thread.request_stop();
        m_state->wake();
        m_state->thread.join();
    }
    // Ended explicitly: a worker that outlives the core on purpose has no KILL_ON_JOB_CLOSE.
    if (m_state)
    {
        for (auto& [name, worker] : m_state->workers)
        {
            worker->process.terminate();
        }
    }
}

Result<void> Supervisor::start(WorkerSpec spec)
{
    Result<void> started;
    {
        const std::scoped_lock lock(m_state->mutex);
        auto& slot = m_state->workers[spec.name];
        if (slot)
        {
            slot->process.terminate(); // the one it replaces
            if (slot->leaving && slot->key != 0)
            {
                // Replaced before it was done leaving: its owner puts back what it could not.
                m_state->pending.push_back(
                    {slot->spec.name, WorkerEventKind::Left, WorkerFailure::Hung, slot->process.pid(), 1});
                m_state->left.notify_all();
            }
        }
        slot = std::make_unique<Worker>(std::move(spec));
        started = m_state->launch(*slot);
        if (!started)
        {
            m_state->recordFailure(*slot, WorkerFailure::StartFailed, 0, 0);
        }
    }
    m_state->wake();
    return started;
}

void Supervisor::stop(std::string_view name)
{
    std::unique_ptr<Worker> gone;
    {
        const std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->workers.find(name);
        if (found == m_state->workers.end())
        {
            return;
        }
        gone = std::move(found->second);
        m_state->workers.erase(found);
        m_state->left.notify_all();
    }
    gone->process.terminate();
    m_state->wake();
    log::info("worker {} stopped", name);
}

bool Supervisor::leave(std::string_view name, const std::function<void(std::uint32_t pid)>& ask,
                       std::chrono::milliseconds patience)
{
    std::uint32_t pid = 0;
    {
        const std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->workers.find(name);
        if (found == m_state->workers.end())
        {
            return false;
        }
        Worker& worker = *found->second;
        if (worker.key == 0)
        {
            // Not running (waiting for a restart, or given up on): nothing to ask.
            m_state->workers.erase(found);
            m_state->left.notify_all();
            log::info("worker {} stopped", name);
            return false;
        }
        const auto deadline = Clock::now() + patience;
        worker.leaveBy = worker.leaving ? std::min(worker.leaveBy, deadline) : deadline;
        worker.leaving = true;
        worker.restartAt.reset();
        pid = worker.process.pid();
    }
    log::info("worker {}: asked to leave", name);
    if (ask)
    {
        try
        {
            ask(pid);
        }
        catch (const std::exception& e)
        {
            log::warn("worker {}: asking it to leave failed: {}", name, e.what());
        }
    }
    m_state->wake();
    return true;
}

bool Supervisor::waitLeft(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_state->mutex);
    return m_state->left.wait_for(lock, timeout, [this] { return !m_state->anyLeaving(); });
}

void Supervisor::setSuspended(bool suspended)
{
    {
        const std::scoped_lock lock(m_state->mutex);
        if (m_state->suspended == suspended)
        {
            return;
        }
        m_state->suspended = suspended;
        const auto now = Clock::now();
        for (auto& [name, worker] : m_state->workers)
        {
            if (suspended)
            {
                worker->watch.pause();
            }
            else
            {
                worker->watch.resume(now);
            }
        }
    }
    log::info("workers: {}",
              suspended ? "the PC goes to sleep; no hang checks" : "the PC is back; hang checks resume");
    m_state->wake();
}

std::vector<WorkerStatus> Supervisor::status() const
{
    const std::scoped_lock lock(m_state->mutex);
    std::vector<WorkerStatus> list;
    for (const auto& [name, worker] : m_state->workers)
    {
        list.push_back({.name = name,
                        .running = worker->key != 0,
                        .disabled = worker->disabled,
                        .pid = worker->key != 0 ? worker->process.pid() : 0,
                        .restarts = worker->restarts,
                        .lastFailure = worker->lastFailure});
    }
    return list;
}

// ---- worker side ------------------------------------------------------------------------------

Result<Heartbeat> Heartbeat::fromArguments(std::span<const std::wstring_view> arguments)
{
    Heartbeat heartbeat;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i)
    {
        if (arguments[i] != L"--heartbeat")
        {
            continue;
        }
        const std::string text = utf16To8Lossy(arguments[i + 1]);
        std::uintptr_t value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0)
        {
            return fail(ErrorCode::InvalidArgument, "--heartbeat takes a handle value");
        }
        auto* const mapping = reinterpret_cast<HANDLE>(value); // NOLINT(performance-no-int-to-ptr): inherited
        void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(std::int64_t));
        if (view == nullptr)
        {
            return std::unexpected(lastError("MapViewOfFile(heartbeat)"));
        }
        heartbeat.m_mapping = mapping;
        heartbeat.m_counter = static_cast<volatile std::int64_t*>(view);
        break;
    }
    return heartbeat;
}

Heartbeat::~Heartbeat()
{
    if (m_counter != nullptr)
    {
        UnmapViewOfFile(const_cast<std::int64_t*>(m_counter));
    }
    if (m_mapping != nullptr)
    {
        CloseHandle(m_mapping);
    }
}

Heartbeat::Heartbeat(Heartbeat&& other) noexcept
    : m_mapping(std::exchange(other.m_mapping, nullptr))
    , m_counter(std::exchange(other.m_counter, nullptr))
{
}

Heartbeat& Heartbeat::operator=(Heartbeat&& other) noexcept
{
    if (this != &other)
    {
        const Heartbeat old(std::move(*this));
        m_mapping = std::exchange(other.m_mapping, nullptr);
        m_counter = std::exchange(other.m_counter, nullptr);
    }
    return *this;
}

void Heartbeat::beat() noexcept
{
    if (m_counter != nullptr)
    {
        InterlockedIncrement64(m_counter);
    }
}

} // namespace rmw::platform
