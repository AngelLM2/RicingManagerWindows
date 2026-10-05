#include "rmw/video/Plan.hpp"
#include "rmw/video/Playback.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using namespace rmw;
using video::PauseReason;

domain::MonitorSelector selector(std::string_view text)
{
    return domain::MonitorSelector::parse(text).value();
}

TEST(VideoPlan, RoundTripsAndChecksItsInput)
{
    video::Plan plan;
    plan.entries.push_back({selector("primary"),
                            LR"(C:\Users\x\AppData\Local\RicingManagerWindows\store\ab\cd.mp4)",
                            domain::WallpaperFit::Fit});
    plan.entries.push_back({selector("all"), {}, domain::WallpaperFit::Fill});
    plan.policy.fpsLimit = 24;
    plan.policy.pauseOnBattery = false;
    const auto parsed = video::planFromJson(video::toJson(plan));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_EQ(*parsed, plan);
    EXPECT_TRUE(parsed->hasVideo());

    for (const std::string_view bad : {
             R"({"version":2,"entries":[]})",
             R"({"version":1,"entries":{}})",
             R"({"version":1,"entries":[{"monitor":"nowhere","file":""}]})",
             R"({"version":1,"entries":[{"monitor":"all","file":"relative\\video.mp4"}]})",
             R"({"version":1,"entries":[{"monitor":"all","file":"","fit":"tile"}]})",
             R"([])",
         })
    {
        EXPECT_FALSE(video::planFromJson(bad).has_value()) << bad;
    }
    // A limit out of range is clamped, not trusted.
    const auto clamped = video::planFromJson(R"({"version":1,"entries":[],"policy":{"fps_limit":100000}})");
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->policy.fpsLimit, 240u);
}

TEST(VideoPlan, OtherRulesClaimTheirMonitors)
{
    // The rice: the primary monitor shows an image, every other one the video.
    video::Plan plan;
    plan.entries.push_back({selector("primary"), {}, domain::WallpaperFit::Fill});
    plan.entries.push_back({selector("all"), LR"(C:\v.mp4)", domain::WallpaperFit::Fill});
    const std::vector<domain::MonitorFacts> monitors = {{"a", true, 0, 0, 1920, 1080},
                                                        {"b", false, 1920, 0, 3840, 1080}};
    EXPECT_EQ(video::videoFor(plan, monitors), (std::vector<int>{-1, 1}));
}

TEST(VideoPlayback, PausesForTheRightReason)
{
    const video::Policy policy;
    EXPECT_EQ(video::pauseReason(policy, {}, {}), PauseReason::None);
    EXPECT_EQ(video::pauseReason(policy, {}, {.fullscreen = true, .covered = true}), PauseReason::Fullscreen);
    EXPECT_EQ(video::pauseReason(policy, {.onBattery = true}, {}), PauseReason::Battery);
    EXPECT_EQ(video::pauseReason(policy, {.batterySaver = true}, {}), PauseReason::Battery);
    EXPECT_EQ(video::pauseReason(policy, {}, {.covered = true}), PauseReason::Covered);
    EXPECT_EQ(video::pauseReason(policy, {.locked = true}, {.fullscreen = true}), PauseReason::Locked);
    EXPECT_EQ(video::pauseReason(policy, {.locked = true, .displayOff = true}, {}), PauseReason::DisplayOff);
    EXPECT_EQ(video::pauseReason(policy, {.suspending = true}, {}), PauseReason::Suspending);

    // What the rice turns off does not pause; what is about the session always does.
    video::Policy relaxed;
    relaxed.pauseOnFullscreen = false;
    relaxed.pauseOnBattery = false;
    relaxed.pauseWhenCovered = false;
    EXPECT_EQ(video::pauseReason(relaxed, {.onBattery = true}, {.fullscreen = true, .covered = true}),
              PauseReason::None);
    EXPECT_EQ(video::pauseReason(relaxed, {.locked = true}, {}), PauseReason::Locked);
}

TEST(VideoPlayback, PlacesTheFrame)
{
    using domain::WallpaperFit;
    using video::Rect;
    // 16:9 video on a 16:10 monitor.
    const auto fill = video::place(WallpaperFit::Fill, 1920, 1080, 1920, 1200);
    EXPECT_EQ(fill.target, (Rect{0, 0, 1920, 1200}));
    EXPECT_EQ(fill.source.height(), 1080);
    EXPECT_EQ(fill.source.width(), 1728); // the sides are cropped
    EXPECT_EQ(fill.source.left, 96);

    const auto fit = video::place(WallpaperFit::Fit, 1920, 1080, 1920, 1200);
    EXPECT_EQ(fit.source, (Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(fit.target, (Rect{0, 60, 1920, 1140})); // bars above and below

    EXPECT_EQ(video::place(WallpaperFit::Stretch, 1280, 720, 1920, 1200).target, (Rect{0, 0, 1920, 1200}));

    const auto small = video::place(WallpaperFit::Center, 1280, 720, 1920, 1080);
    EXPECT_EQ(small.source, (Rect{0, 0, 1280, 720}));
    EXPECT_EQ(small.target, (Rect{320, 180, 1600, 900}));
    const auto large = video::place(WallpaperFit::Center, 3840, 2160, 1920, 1080);
    EXPECT_EQ(large.source, (Rect{960, 540, 2880, 1620}));
    EXPECT_EQ(large.target, (Rect{0, 0, 1920, 1080}));

    // Nonsense sizes never divide by zero.
    EXPECT_EQ(video::place(WallpaperFit::Fill, 0, 0, 1920, 1080).target, (Rect{0, 0, 1920, 1080}));
}

TEST(VideoPlayback, PacesFrames)
{
    video::FramePacer pacer(30);
    const auto start = video::Clock::now();
    EXPECT_TRUE(pacer.due(start));
    EXPECT_FALSE(pacer.due(start + std::chrono::milliseconds(10)));
    EXPECT_TRUE(pacer.due(start + std::chrono::milliseconds(34)));
    // Back from a long pause: one frame, not a burst to catch up.
    EXPECT_TRUE(pacer.due(start + std::chrono::seconds(5)));
    EXPECT_FALSE(pacer.due(start + std::chrono::seconds(5) + std::chrono::milliseconds(1)));

    video::FramePacer unlimited(0);
    EXPECT_TRUE(unlimited.due(start));
    EXPECT_TRUE(unlimited.due(start));
}

} // namespace
