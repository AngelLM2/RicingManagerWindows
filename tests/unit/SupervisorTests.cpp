#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Supervisor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
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

TEST(Heartbeat, WithoutTheFlagItDoesNothing)
{
    auto heartbeat = Heartbeat::fromArguments({});
    ASSERT_TRUE(heartbeat.has_value());
    heartbeat->beat();
    const std::vector<std::wstring_view> bad{L"--heartbeat", L"nope"};
    EXPECT_FALSE(Heartbeat::fromArguments(bad).has_value());
}

} // namespace
