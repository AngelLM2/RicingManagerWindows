#include "TempDir.hpp"

#include "rmw/platform/MessageWindow.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using rmw::platform::MessageWindow;
using rmw::platform::SystemEvent;
using rmw::platform::SystemEventKind;

// Runs a window on a thread of its own (it must pump where it was made); -1 when run() never
// returned (the thread is left behind: the loop is stuck).
int runWindow(const std::function<void(MessageWindow&)>& setUp, std::vector<SystemEvent>& events)
{
    std::promise<int> exit;
    auto result = exit.get_future();
    std::thread thread([&setUp, &events, done = std::move(exit)]() mutable {
        auto window = MessageWindow::create([&events](const SystemEvent& event) { events.push_back(event); });
        if (!window)
        {
            done.set_value(-2);
            return;
        }
        setUp(**window);
        done.set_value((*window)->run());
    });
    if (result.wait_for(10s) != std::future_status::ready)
    {
        thread.detach();
        return -1;
    }
    thread.join();
    return result.get();
}

// The review of v1.7: a watched folder that went away kept its handle signaled, and the loop served
// it forever without pumping a message (100% CPU, no hotkeys, no tray, no quit).
TEST(MessageWindow, AWatchedFolderThatGoesAwayDoesNotStopTheLoop)
{
    const rmw::test::TempDir dir;
    const auto folder = dir.path() / "rice";
    std::filesystem::create_directories(folder);
    std::vector<SystemEvent> events;
    const int code = runWindow(
        [&](MessageWindow& window) {
            ASSERT_TRUE(window.watchFolder(7, folder).has_value());
            std::filesystem::remove_all(folder);
            window.post([&window] { window.quit(3); });
        },
        events);
    EXPECT_EQ(code, 3);
}

TEST(MessageWindow, ATaskThatThrowsDoesNotDropTheOthers)
{
    std::vector<SystemEvent> events;
    bool ranAfter = false;
    const int code = runWindow(
        [&](MessageWindow& window) {
            window.post([] { throw std::runtime_error("a task fails"); });
            window.post([&ranAfter] { ranAfter = true; });
            window.post([&window] { window.quit(5); });
        },
        events);
    EXPECT_EQ(code, 5);
    EXPECT_TRUE(ranAfter);
}

} // namespace
