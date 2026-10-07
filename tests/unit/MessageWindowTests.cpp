#include "TempDir.hpp"

#include "rmw/platform/MessageWindow.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

constexpr std::uint32_t kQuitTimer = 41;

// Runs a window on a thread of its own (it must pump where it was made) until the kQuitTimer timer
// or a quit(); -1 when run() never returned (the thread is left behind: the loop is stuck).
int runWindow(const std::function<void(MessageWindow&)>& setUp, std::vector<SystemEvent>& events)
{
    std::promise<int> exit;
    auto result = exit.get_future();
    std::thread thread([&setUp, &events, done = std::move(exit)]() mutable {
        MessageWindow* self = nullptr;
        auto window = MessageWindow::create([&](const SystemEvent& event) {
            events.push_back(event);
            if (event.kind == SystemEventKind::Timer && event.id == kQuitTimer && self != nullptr)
            {
                self->quit(9);
            }
        });
        if (!window)
        {
            done.set_value(-2);
            return;
        }
        self = window->get();
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

std::size_t count(const std::vector<SystemEvent>& events, SystemEventKind kind)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(events, [kind](const SystemEvent& event) { return event.kind == kind; }));
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
            window.setTimer(kQuitTimer, 500ms);
        },
        events);
    EXPECT_EQ(code, 9); // the timer still came
    // The removal was seen, and the watch went with its folder instead of firing again and again.
    EXPECT_GE(count(events, SystemEventKind::FolderChanged), 1u);
    EXPECT_LE(count(events, SystemEventKind::FolderChanged), 2u);
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
