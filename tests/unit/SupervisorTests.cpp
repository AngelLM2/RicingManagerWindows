#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/Supervisor.hpp"

#include <windows.h>

#include <shellapi.h>

#include <gtest/gtest.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwctype>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Real processes (tests/support/TestWorker.cpp) in real jobs: what docs/03 §2.4 promises.
namespace
{

using namespace rmw::platform;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class EventLog
{
public:
    void add(const WorkerEvent& event)
    {
        const std::scoped_lock lock(m_mutex);
        m_events.push_back(event);
        m_changed.notify_all();
    }

    [[nodiscard]] int count(WorkerEventKind kind, WorkerFailure failure = WorkerFailure::None) const
    {
        const std::scoped_lock lock(m_mutex);
        return static_cast<int>(std::ranges::count_if(m_events, [&](const WorkerEvent& event) {
            return event.kind == kind && (failure == WorkerFailure::None || event.failure == failure);
        }));
    }

    // Waits until `kind` has happened `times` times.
    [[nodiscard]] bool waitFor(WorkerEventKind kind, int times = 1, std::chrono::milliseconds timeout = 30s)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] {
            return std::ranges::count_if(
                       m_events, [&](const WorkerEvent& event) { return event.kind == kind; }) >= times;
        });
    }

    [[nodiscard]] std::vector<WorkerEvent> all() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_events;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<WorkerEvent> m_events;
};

WorkerSpec testWorker(std::wstring_view mode)
{
    WorkerSpec spec;
    spec.name = "test";
    spec.executable = currentExecutable().value().parent_path() / L"rmw_test_worker.exe";
    spec.arguments = std::format(L"--mode {}", mode);
    spec.memoryLimitBytes = std::uint64_t{96} << 20;
    spec.hangAfter = 600ms;
    spec.startGrace = 5s;
    spec.firstBackoff = 50ms;
    return spec;
}

struct Running
{
    std::shared_ptr<EventLog> events = std::make_shared<EventLog>();
    std::unique_ptr<Supervisor> supervisor;

    explicit Running(std::wstring_view mode)
    {
        auto created = Supervisor::create([log = events](const WorkerEvent& event) { log->add(event); });
        EXPECT_TRUE(created.has_value());
        if (created)
        {
            supervisor = std::move(*created);
            EXPECT_TRUE(supervisor->start(testWorker(mode)).has_value());
        }
    }
};

TEST(RestartBudget, BacksOffThenGivesUp)
{
    const auto t0 = Clock::time_point{} + 24h;
    RestartBudget budget(3, 5min, 1s);
    EXPECT_EQ(budget.onFailure(t0), 1000ms);
    EXPECT_EQ(budget.onFailure(t0 + 10s), 2000ms);
    EXPECT_FALSE(budget.onFailure(t0 + 20s).has_value());

    // Failures older than the window are forgotten.
    RestartBudget spaced(3, 5min, 1s);
    EXPECT_EQ(spaced.onFailure(t0), 1000ms);
    EXPECT_EQ(spaced.onFailure(t0 + 4min), 2000ms);
    EXPECT_EQ(spaced.onFailure(t0 + 8min), 2000ms); // t0 is out of the window, t0 + 4 min is not
}

TEST(Supervisor, LeakingWorkerIsEndedRestartedThenDisabled)
{
#if defined(__SANITIZE_ADDRESS__)
    GTEST_SKIP() << "no memory limits under ASan";
#endif
    const Running run(L"leak");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Disabled));
    EXPECT_EQ(run.events->count(WorkerEventKind::Started), 3);
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed, WorkerFailure::MemoryLimit), 3);
    const auto status = run.supervisor->status();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_TRUE(status[0].disabled);
    EXPECT_FALSE(status[0].running);
    EXPECT_EQ(status[0].restarts, 2);
}

TEST(Supervisor, HungWorkerIsEndedAndRestarted)
{
    const Running run(L"hang");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Started, 2));
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed, WorkerFailure::Hung), 1);
    run.supervisor->stop("test");
    EXPECT_TRUE(run.supervisor->status().empty());
}

TEST(Supervisor, CrashedWorkerIsRestarted)
{
    const Running run(L"crash");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Started, 2));
    const auto events = run.events->all();
    const auto crashed = std::ranges::find_if(events, [](const WorkerEvent& event) {
        return event.kind == WorkerEventKind::Failed && event.failure == WorkerFailure::Crashed;
    });
    ASSERT_NE(crashed, events.end());
    EXPECT_EQ(crashed->exitCode, 0xC0000005u);
}

TEST(Supervisor, CleanExitIsNotRestarted)
{
    const Running run(L"exit");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Exited));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(run.events->count(WorkerEventKind::Started), 1);
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed), 0);
}

TEST(Supervisor, HealthyWorkerRunsUntilStopped)
{
    const Running run(L"ok");
    ASSERT_TRUE(run.supervisor);
    std::this_thread::sleep_for(1500ms); // well past hangAfter
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed), 0);
    const auto status = run.supervisor->status();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_TRUE(status[0].running);
    EXPECT_NE(status[0].pid, 0u);
    run.supervisor->stop("test");
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed), 0); // stopping is not a failure
}

TEST(Supervisor, MissingExecutableFailsToStartAndIsRetried)
{
    auto events = std::make_shared<EventLog>();
    auto supervisor = Supervisor::create([events](const WorkerEvent& event) { events->add(event); });
    ASSERT_TRUE(supervisor.has_value());
    WorkerSpec spec = testWorker(L"ok");
    spec.executable = spec.executable.parent_path() / L"no-such-worker.exe";
    EXPECT_FALSE((*supervisor)->start(spec).has_value());
    ASSERT_TRUE(events->waitFor(WorkerEventKind::Disabled));
    EXPECT_EQ(events->count(WorkerEventKind::Failed, WorkerFailure::StartFailed), 3);
}

TEST(Supervisor, AWorkerThatOutlivesTheCoreIsStillEndedOnStop)
{
    auto events = std::make_shared<EventLog>();
    auto supervisor = Supervisor::create([events](const WorkerEvent& event) { events->add(event); });
    ASSERT_TRUE(supervisor.has_value());
    WorkerSpec spec = testWorker(L"ok");
    spec.endsWithCore = false; // gets --core: the test process
    ASSERT_TRUE((*supervisor)->start(spec).has_value());
    ASSERT_TRUE(events->waitFor(WorkerEventKind::Started));
    std::this_thread::sleep_for(800ms);
    const auto status = (*supervisor)->status();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_TRUE(status[0].running); // --core was understood: it did not exit
    (*supervisor)->stop("test");
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(events->count(WorkerEventKind::Failed), 0);
}

// Asks the test worker to leave the way a window is asked: its thread's loop gets WM_QUIT and it returns 0.
void askToQuit(std::uint32_t pid)
{
    auto* const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more != FALSE; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID == pid)
        {
            PostThreadMessageW(entry.th32ThreadID, WM_QUIT, 0, 0);
        }
    }
    CloseHandle(snapshot);
}

// The lifecycle review (v1.8.1): every exit ended the bar outright, so it never gave back the strip it had
// set aside. Asked, a worker leaves by itself; its exit is no failure and it is not started again.
TEST(Supervisor, AWorkerAskedToLeaveLeavesAndIsNotRestarted)
{
    const Running run(L"ok");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Started));
    std::this_thread::sleep_for(300ms); // its message loop is up
    EXPECT_TRUE(run.supervisor->leave("test", &askToQuit, 5s));
    EXPECT_TRUE(run.supervisor->waitLeft(10s));
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Left));
    const auto events = run.events->all();
    const auto left = std::ranges::find(events, WorkerEventKind::Left, &WorkerEvent::kind);
    ASSERT_NE(left, events.end());
    EXPECT_EQ(left->failure, WorkerFailure::None);
    EXPECT_EQ(left->exitCode, 0u);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(run.events->count(WorkerEventKind::Started), 1);
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed), 0);
    EXPECT_TRUE(run.supervisor->status().empty());
}

TEST(Supervisor, AWorkerThatDoesNotLeaveIsEndedWhenItsTimeIsUp)
{
    const Running run(L"ok");
    ASSERT_TRUE(run.supervisor);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Started));
    const auto asked = Clock::now();
    EXPECT_TRUE(run.supervisor->leave("test", [](std::uint32_t) {}, 400ms)); // deaf to it
    EXPECT_TRUE(run.supervisor->waitLeft(10s));
    EXPECT_GE(Clock::now() - asked, 400ms);
    ASSERT_TRUE(run.events->waitFor(WorkerEventKind::Left));
    const auto events = run.events->all();
    const auto left = std::ranges::find(events, WorkerEventKind::Left, &WorkerEvent::kind);
    ASSERT_NE(left, events.end());
    EXPECT_EQ(left->failure, WorkerFailure::Hung); // its owner puts back what it could not
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(run.events->count(WorkerEventKind::Started), 1);
    EXPECT_EQ(run.events->count(WorkerEventKind::Failed), 0);
}

TEST(Supervisor, LeavingWhatIsNotRunningIsNothing)
{
    const Running run(L"ok");
    ASSERT_TRUE(run.supervisor);
    EXPECT_FALSE(run.supervisor->leave("nobody", [](std::uint32_t) {}, 1s));
    EXPECT_TRUE(run.supervisor->waitLeft(100ms));
    run.supervisor->stop("test");
}

TEST(HangWatch, SilenceAfterBeatsIsAHang)
{
    const auto t0 = Clock::time_point{} + 24h;
    HangWatch watch(1000ms, 5000ms);
    watch.start(t0);
    EXPECT_FALSE(watch.hung(1, t0 + 250ms)); // a beat
    for (int tick = 2; tick <= 5; ++tick)
    {
        EXPECT_FALSE(watch.hung(1, t0 + tick * 250ms)) << tick; // silent, within a second
    }
    EXPECT_TRUE(watch.hung(1, t0 + 1500ms));
}

TEST(HangWatch, BeforeTheFirstBeatTheStartGraceCounts)
{
    const auto t0 = Clock::time_point{} + 24h;
    HangWatch watch(1000ms, 5000ms);
    watch.start(t0);
    for (auto at = 250ms; at <= 5000ms; at += 250ms)
    {
        EXPECT_FALSE(watch.hung(0, t0 + at)) << at.count();
    }
    EXPECT_TRUE(watch.hung(0, t0 + 5250ms));
}

// The PC slept: the supervisor's clock went on, but it did not look and the worker could not beat. A
// gap between two looks longer than the silence allowed is a new grace, not a hang.
TEST(HangWatch, ASleepIsNotSilence)
{
    const auto t0 = Clock::time_point{} + 24h;
    HangWatch watch(1000ms, 5000ms);
    watch.start(t0);
    EXPECT_FALSE(watch.hung(1, t0 + 250ms));
    const auto woke = t0 + 2h;
    EXPECT_FALSE(watch.hung(1, woke)); // the first look after the gap
    for (auto at = 250ms; at <= 5000ms; at += 250ms)
    {
        EXPECT_FALSE(watch.hung(1, woke + at)) << at.count(); // catching up, still silent
    }
    EXPECT_TRUE(watch.hung(1, woke + 5250ms)); // a real hang is still found
    // And a worker that beats again is watched as before.
    HangWatch beating(1000ms, 5000ms);
    beating.start(t0);
    EXPECT_FALSE(beating.hung(1, t0 + 250ms));
    EXPECT_FALSE(beating.hung(1, woke));
    EXPECT_FALSE(beating.hung(2, woke + 250ms));
    EXPECT_FALSE(beating.hung(2, woke + 1000ms));
    EXPECT_TRUE(beating.hung(2, woke + 1500ms));
}

TEST(HangWatch, NoVerdictWhileThePcSleeps)
{
    const auto t0 = Clock::time_point{} + 24h;
    HangWatch watch(1000ms, 5000ms);
    watch.start(t0);
    EXPECT_FALSE(watch.hung(1, t0 + 250ms));
    watch.pause();
    for (auto at = 500ms; at <= 10s; at += 250ms)
    {
        EXPECT_FALSE(watch.hung(1, t0 + at)) << at.count();
    }
    const auto back = t0 + 10s;
    watch.resume(back); // a new grace: the worker catches up first
    for (auto at = 250ms; at <= 5000ms; at += 250ms)
    {
        EXPECT_FALSE(watch.hung(1, back + at)) << at.count();
    }
    EXPECT_TRUE(watch.hung(1, back + 5250ms));
}

TEST(HangWatch, ZeroMeansNotWatched)
{
    const auto t0 = Clock::time_point{} + 24h;
    HangWatch watch(0ms, 5000ms);
    watch.start(t0);
    EXPECT_FALSE(watch.hung(0, t0 + 1h));
}

TEST(ProcessesFromFolder, FindsOnlyThisFoldersCopies)
{
    const auto self = currentExecutable().value();
    const std::wstring name = self.filename().native();
    constexpr std::wstring_view kOther = L"no-such-program.exe";
    const std::array<std::wstring_view, 2> names = {name, kOther};
    const auto here = rmw::platform::processesFromFolder(self.parent_path(), names);
    EXPECT_TRUE(std::ranges::contains(here, GetCurrentProcessId()));
    // Any case for the folder and the name, like Windows.
    std::wstring upper = self.parent_path().native();
    std::ranges::transform(upper, upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    EXPECT_TRUE(
        std::ranges::contains(rmw::platform::processesFromFolder(upper, names), GetCurrentProcessId()));
    EXPECT_FALSE(std::ranges::contains(
        rmw::platform::processesFromFolder(self.parent_path().parent_path(), names), GetCurrentProcessId()));
}

TEST(Heartbeat, WithoutTheFlagItDoesNothing)
{
    auto heartbeat = Heartbeat::fromArguments({});
    ASSERT_TRUE(heartbeat.has_value());
    heartbeat->beat();
    const std::vector<std::wstring_view> bad{L"--heartbeat", L"nope"};
    EXPECT_FALSE(Heartbeat::fromArguments(bad).has_value());
}

// The review of v1.7: --data-dir "C:\data\" reached the child as one argument with the rest in it.
TEST(QuoteArgument, ArgumentsComeBackAsTheyWere)
{
    const std::array<std::wstring, 6> arguments{L"C:\\Ricing Manager\\data\\", L"plain", L"", L"say \"hi\"",
                                                L"a\\\\\"b\\", L"\\\\server\\share\\"};
    std::wstring line = L"rmw-core.exe";
    for (const auto& argument : arguments)
    {
        line += L" " + quoteArgument(argument);
    }
    line += L" --last";
    int count = 0;
    wchar_t** parsed = CommandLineToArgvW(line.c_str(), &count);
    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(count, static_cast<int>(arguments.size()) + 2);
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        EXPECT_EQ(std::wstring(parsed[i + 1]), arguments[i]);
    }
    EXPECT_EQ(std::wstring(parsed[count - 1]), L"--last");
    LocalFree(static_cast<HLOCAL>(parsed));
}

} // namespace
