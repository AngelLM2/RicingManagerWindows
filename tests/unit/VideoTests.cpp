#include "rmw/base/Limits.hpp"
#include "rmw/video/Particles.hpp"
#include "rmw/video/Plan.hpp"
#include "rmw/video/Playback.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

using namespace rmw;
using video::PauseReason;

domain::MonitorSelector selector(std::string_view text)
{
    return domain::MonitorSelector::parse(text).value();
}

video::Entry entry(std::string_view monitor, video::Source source, std::filesystem::path file = {},
                   domain::WallpaperFit fit = domain::WallpaperFit::Fill)
{
    video::Entry e;
    e.monitor = selector(monitor);
    e.source = source;
    e.file = std::move(file);
    e.fit = fit;
    return e;
}

TEST(VideoPlan, RoundTripsAndChecksItsInput)
{
    video::Plan plan;
    plan.entries.push_back(entry("primary", video::Source::Video,
                                 LR"(C:\Users\x\AppData\Local\RicingManagerWindows\store\ab\cd.mp4)",
                                 domain::WallpaperFit::Fit));
    plan.entries.back().effects.speed = 0.5;
    plan.entries.back().effects.saturation = 1.25;
    auto picture = entry("left", video::Source::Image, LR"(C:\store\ef.png)", domain::WallpaperFit::Tile);
    picture.effects.blur = 6;
    picture.effects.parallax = 0.4;
    picture.particles.push_back(video::defaultsFor(domain::ParticleKind::Petals));
    picture.particles.push_back(video::defaultsFor(domain::ParticleKind::Magic));
    picture.particles.back().color = {0x10, 0x20, 0x30, 0x80};
    picture.particles.back().count = 7;
    plan.entries.push_back(std::move(picture));
    auto color = entry("right", video::Source::Color);
    color.color = {0x22, 0x11, 0x33};
    color.particles.push_back(video::defaultsFor(domain::ParticleKind::Stars));
    plan.entries.push_back(std::move(color));
    plan.entries.push_back(entry("all", video::Source::None));
    plan.policy.fpsLimit = 24;
    plan.policy.pauseOnBattery = false;
    plan.background = {0x1A, 0x1B, 0x26};
    const auto parsed = video::planFromJson(video::toJson(plan));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_EQ(*parsed, plan);
    EXPECT_TRUE(parsed->hasLive());

    for (const std::string_view bad : {
             R"({"version":3,"entries":[]})",
             R"({"version":2,"entries":{}})",
             R"({"version":2,"entries":[{"monitor":"nowhere","source":"none"}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"film","file":"C:\\v.mp4"}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"video","file":"relative\\video.mp4"}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"video","file":"C:\\v.mp4","fit":"tile"}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color"}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","effects":{"blur":99}}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","effects":{"hue":"x"}}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","particles":[{"kind":"fire"}]}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","particles":[{"kind":"snow","count":100000}]}]})",
             R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","particles":[{"kind":"snow","color":"red"}]}]})",
             R"({"version":1,"entries":[{"monitor":"all","file":"","fit":"tile"}]})",
             R"([])",
         })
    {
        EXPECT_FALSE(video::planFromJson(bad).has_value()) << bad;
    }
    std::string many = R"({"version":2,"entries":[{"monitor":"all","source":"color","color":"#000","particles":[)";
    for (std::size_t i = 0; i <= limits::kWallpaperMaxParticleLayers; ++i)
    {
        many += std::string(i == 0 ? "" : ",") + R"({"kind":"snow"})";
    }
    EXPECT_FALSE(video::planFromJson(many + "]}]}").has_value());
    // A limit out of range is clamped, not trusted.
    const auto clamped = video::planFromJson(R"({"version":2,"entries":[],"policy":{"fps_limit":100000}})");
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->policy.fpsLimit, 240u);
}

TEST(VideoPlan, ReadsThePlansOfVersionOne)
{
    // What an older core stored: a file was a video.
    const auto old = video::planFromJson(
        R"({"version":1,"entries":[{"monitor":"primary","file":"C:\\v.mp4","fit":"fit"},{"monitor":"all","file":""}]})");
    ASSERT_TRUE(old.has_value()) << old.error().message();
    ASSERT_EQ(old->entries.size(), 2u);
    EXPECT_EQ(old->entries[0].source, video::Source::Video);
    EXPECT_EQ(old->entries[0].fit, domain::WallpaperFit::Fit);
    EXPECT_EQ(old->entries[1].source, video::Source::None);
    EXPECT_TRUE(old->entries[0].effects.none());
}

TEST(VideoPlan, OtherRulesClaimTheirMonitors)
{
    // The rice: the primary monitor shows a plain image, every other one the video.
    video::Plan plan;
    plan.entries.push_back(entry("primary", video::Source::None));
    plan.entries.push_back(entry("all", video::Source::Video, LR"(C:\v.mp4)"));
    const std::vector<domain::MonitorFacts> monitors = {{"a", true, 0, 0, 1920, 1080},
                                                        {"b", false, 1920, 0, 3840, 1080}};
    EXPECT_EQ(video::liveFor(plan, monitors), (std::vector<int>{-1, 1}));
}

TEST(VideoEffects, TheColorMatrixDoesWhatItSays)
{
    const domain::Color orange{0xF0, 0x80, 0x20};
    // As it is.
    const video::Effects none;
    EXPECT_TRUE(none.none());
    EXPECT_EQ(video::applyMatrix(video::colorMatrix(none), orange), orange);
    // No saturation: gray, with the luminance.
    video::Effects gray;
    gray.saturation = 0.0;
    const auto g = video::applyMatrix(video::colorMatrix(gray), orange);
    EXPECT_EQ(g.r, g.g);
    EXPECT_EQ(g.g, g.b);
    EXPECT_NEAR(g.r, (0.2126 * 0xF0) + (0.7152 * 0x80) + (0.0722 * 0x20), 1.0);
    // Darker, lighter; no contrast: the middle gray.
    video::Effects dark;
    dark.brightness = -1.0;
    EXPECT_EQ(video::applyMatrix(video::colorMatrix(dark), orange), (domain::Color{0, 0, 0}));
    video::Effects flat;
    flat.contrast = 0.0;
    EXPECT_EQ(video::applyMatrix(video::colorMatrix(flat), orange), (domain::Color{0x80, 0x80, 0x80}));
    // A whole turn of hue is no turn; half a turn keeps a gray gray.
    video::Effects turn;
    turn.hue = 180.0;
    const domain::Color mid{0x80, 0x80, 0x80};
    EXPECT_EQ(video::applyMatrix(video::colorMatrix(turn), mid), mid);
    const auto turned = video::applyMatrix(video::colorMatrix(turn), orange);
    EXPECT_LT(turned.r, turned.b); // orange turns blue-ish
    // Alpha is left alone.
    EXPECT_EQ(video::applyMatrix(video::colorMatrix(dark), domain::Color{1, 2, 3, 0x40}).a, 0x40);
    EXPECT_FALSE(dark.keepsColors());
    video::Effects moving;
    moving.zoom = 0.2;
    EXPECT_TRUE(moving.moves());
    EXPECT_TRUE(moving.keepsColors());
}

TEST(VideoEffects, LayersTakeTheKindsDefaultsAndThePalette)
{
    domain::Rice rice;
    rice.palette.push_back({"glow", {0x11, 0x22, 0x33}});
    domain::ParticleLayer layer;
    layer.kind = domain::ParticleKind::Snow;
    layer.count = 12;
    auto snow = video::particlesOf(layer, rice, {});
    EXPECT_EQ(snow.count, 12u);
    EXPECT_EQ(snow.color, video::defaultsFor(domain::ParticleKind::Snow).color);
    EXPECT_EQ(snow.direction, 90.0);
    layer.color = domain::ColorRef{std::string("glow")};
    EXPECT_EQ(video::particlesOf(layer, rice, {}).color, (domain::Color{0x11, 0x22, 0x33}));
    // "palette.accent": the rice's accent, else Windows'.
    layer.color = domain::ColorRef{std::string("accent")};
    const domain::Color windows{0x00, 0x78, 0xD4};
    EXPECT_EQ(video::particlesOf(layer, rice, windows).color, windows);
    rice.appearance = domain::AppearanceConfig{};
    rice.appearance->accent = domain::Color{0xAA, 0xBB, 0xCC};
    EXPECT_EQ(video::particlesOf(layer, rice, windows).color, (domain::Color{0xAA, 0xBB, 0xCC}));
}

TEST(VideoParticles, StayOnScreenAndMoveTheirWay)
{
    for (const auto kind : {domain::ParticleKind::Sparkles, domain::ParticleKind::Stars, domain::ParticleKind::Snow,
                            domain::ParticleKind::Rain, domain::ParticleKind::Petals, domain::ParticleKind::Fireflies,
                            domain::ParticleKind::Magic, domain::ParticleKind::Notes, domain::ParticleKind::Hearts,
                            domain::ParticleKind::Bubbles})
    {
        auto layer = video::defaultsFor(kind);
        layer.count = 50;
        video::ParticleField field(layer, 1920, 1080, 7);
        ASSERT_EQ(field.sprites().size(), 50u);
        for (int frame = 0; frame < 600; ++frame) // 20 s at 30 fps
        {
            field.step(1.0 / 30.0);
        }
        const float margin = field.largest() + 80.0F;
        for (const auto& sprite : field.sprites())
        {
            EXPECT_GE(sprite.x, -margin) << domain::toString(kind);
            EXPECT_LE(sprite.x, 1920.0F + margin) << domain::toString(kind);
            EXPECT_GE(sprite.y, -margin) << domain::toString(kind);
            EXPECT_LE(sprite.y, 1080.0F + margin) << domain::toString(kind);
            EXPECT_GE(sprite.opacity, 0.0F);
            EXPECT_LE(sprite.opacity, 1.0F);
            EXPECT_GT(sprite.size, 0.0F);
        }
    }

    // Snow falls: after a short step, every flake is lower (the sway is across, so x may change).
    auto snow = video::defaultsFor(domain::ParticleKind::Snow);
    snow.count = 20;
    video::ParticleField falling(snow, 1920, 1080, 3);
    std::vector<float> before;
    for (const auto& sprite : falling.sprites())
    {
        before.push_back(sprite.y);
    }
    falling.step(0.05);
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_GT(falling.sprites()[i].y, before[i]);
    }
    // Upwards when the rice says so; frozen at speed 0; the same seed, the same field.
    snow.direction = 270.0;
    video::ParticleField rising(snow, 1920, 1080, 3);
    const float start = rising.sprites()[0].y;
    rising.step(0.05);
    EXPECT_LT(rising.sprites()[0].y, start);
    snow.speed = 0.0;
    video::ParticleField frozen(snow, 1920, 1080, 3);
    const auto still = frozen.sprites();
    frozen.step(0.1);
    EXPECT_TRUE(frozen.still());
    EXPECT_EQ(frozen.sprites()[0].y, still[0].y);
    const video::ParticleField again(snow, 1920, 1080, 3);
    EXPECT_EQ(again.sprites()[5].x, still[5].x);
    // Bigger monitors, bigger particles.
    snow.speed = 1.0;
    const video::ParticleField small(snow, 1920, 1080, 1);
    const video::ParticleField large(snow, 3840, 2160, 1);
    EXPECT_FLOAT_EQ(large.largest(), 2.0F * small.largest());
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
