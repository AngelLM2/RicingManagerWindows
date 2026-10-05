#include "rmw/bar/Geometry.hpp"
#include "rmw/bar/Plan.hpp"
#include "rmw/bar/Source.hpp"
#include "rmw/domain/RiceParser.hpp"

#include <gtest/gtest.h>

#include <array>

namespace
{

using namespace rmw::bar;
using rmw::domain::BarConfig;
using rmw::domain::BarPosition;

const Rect kMonitor{0, 0, 1920, 1080};

BarConfig bottomBar()
{
    BarConfig bar;
    bar.position = BarPosition::Bottom;
    bar.thickness = 40;
    return bar;
}

TEST(BarGeometry, AFullBottomBarTakesItsStrip)
{
    const BarConfig bar = bottomBar();
    const Rect strip = reservation(bar, kMonitor, 1.0);
    EXPECT_EQ(strip, (Rect{0, 1040, 1920, 1080}));
    EXPECT_EQ(windowRect(bar, strip, 1.0), (Rect{0, 1040, 1920, 1080}));
}

TEST(BarGeometry, MarginsLengthAndScale)
{
    BarConfig bar = bottomBar();
    bar.margin = {6, 8, 6, 8}; // top, right, bottom, left
    bar.length = {true, 50.0};
    // 150%: 40 + 6 + 6 logical = 78 px deep.
    const Rect strip = reservation(bar, kMonitor, 1.5);
    EXPECT_EQ(strip, (Rect{0, 1002, 1920, 1080}));
    const Rect window = windowRect(bar, strip, 1.5);
    EXPECT_EQ(window.height(), 60);
    EXPECT_EQ(window.bottom, 1080 - 9);
    EXPECT_EQ(window.width(), 960); // half the edge
    EXPECT_EQ(window.left, (1920 - 960) / 2);

    bar.length = {false, 4000.0}; // wider than the screen: clamped inside the margins
    const Rect clamped = windowRect(bar, strip, 1.0);
    EXPECT_EQ(clamped.left, 8);
    EXPECT_EQ(clamped.right, 1920 - 8);
}

TEST(BarGeometry, VerticalAndTopBars)
{
    BarConfig left = bottomBar();
    left.position = BarPosition::Left;
    left.thickness = 48;
    left.margin = {10, 0, 10, 4};
    const Rect strip = reservation(left, Rect{1920, 0, 3840, 1080}, 1.0);
    EXPECT_EQ(strip, (Rect{1920, 0, 1972, 1080}));
    const Rect window = windowRect(left, strip, 1.0);
    EXPECT_EQ(window, (Rect{1924, 10, 1972, 1070}));
    EXPECT_FALSE(isHorizontal(BarPosition::Left));

    BarConfig top = bottomBar();
    top.position = BarPosition::Top;
    top.mode = rmw::domain::BarMode::Float;
    top.margin = {12, 0, 0, 0};
    // Floating: placed inside the work area, nothing reserved.
    const Rect work{0, 0, 1920, 1032};
    EXPECT_EQ(windowRect(top, work, 1.0), (Rect{0, 12, 1920, 52}));
}

TEST(BarPlan, RoundTripsThroughJsonWithColorsResolved)
{
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(R"toml(
schema = 1
[meta]
name = "Night"
[palette]
bg = "#1a1b26"
fg = "#c0caf5"
[[bar]]
monitor = "primary"
position = "top"
thickness = 32
margin = [4, 8]
radius = 10
background = { kind = "acrylic", tint = "palette.bg", opacity = 0.8 }
foreground = "palette.fg"
hide_native_taskbar = true
left = ["start", { id = "clock", format = "%H:%M", seconds = false }]
right = ["windows"]
)toml",
                                             diagnostics);
    ASSERT_TRUE(rice.has_value()) << rice.error().toString();
    const auto plan = planFromRice(*rice);
    ASSERT_TRUE(plan.has_value()) << plan.error().toString();
    EXPECT_TRUE(hidesNativeTaskbar(*plan));

    const auto back = planFromJson(toJson(*plan));
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    ASSERT_EQ(back->bars.size(), 1u);
    const BarConfig& bar = back->bars[0];
    EXPECT_EQ(back->riceName, "Night");
    EXPECT_EQ(bar.position, BarPosition::Top);
    EXPECT_EQ(bar.thickness, 32u);
    EXPECT_EQ(bar.margin, (std::array<std::uint32_t, 4>{4, 8, 4, 8}));
    EXPECT_EQ(bar.background.kind, rmw::domain::BarBackground::Kind::Acrylic);
    EXPECT_EQ(bar.background.tint->toString(), "#1A1B26");
    EXPECT_EQ(bar.foreground->toString(), "#C0CAF5");
    EXPECT_DOUBLE_EQ(*bar.background.opacity, 0.8);
    ASSERT_EQ(bar.left.size(), 2u);
    EXPECT_EQ(bar.left[1].id, "clock");
    EXPECT_EQ(bar.left[1].config, plan->bars[0].left[1].config); // settings survive as they were
    EXPECT_EQ(bar.right[0].id, "windows");
}

TEST(BarPlan, RefusesBadPlans)
{
    EXPECT_FALSE(planFromJson("{").has_value());
    EXPECT_FALSE(planFromJson(R"({"version": 2, "bars": []})").has_value());
    EXPECT_EQ(planFromJson(R"({"version": 1, "bars": []})")->bars.size(), 0u);
    const char* bad[] = {
        R"({"version":1,"bars":[{"position":"diagonal","mode":"reserve","thickness":40,"length":{"value":100},"margin":[0,0,0,0],"background":{"kind":"solid"}}]})",
        R"({"version":1,"bars":[{"position":"top","mode":"reserve","thickness":4,"length":{"value":100},"margin":[0,0,0,0],"background":{"kind":"solid"}}]})",
        R"({"version":1,"bars":[{"position":"top","mode":"reserve","thickness":40,"length":{"value":100},"margin":[0,0],"background":{"kind":"solid"}}]})",
        R"({"version":1,"bars":[{"position":"top","mode":"reserve","thickness":40,"length":{"value":100},"margin":[0,0,0,0],"background":{"kind":"solid","tint":"red"}}]})",
        R"({"version":1,"bars":[{"position":"top","mode":"reserve","thickness":40,"length":{"value":100},"margin":[0,0,0,0],"background":{"kind":"solid"},"left":[{"config":{}}]}]})",
    };
    for (const char* text : bad)
    {
        EXPECT_FALSE(planFromJson(text).has_value()) << text;
    }
}

constexpr std::array<WidgetProperty, 1> kClockProperties = {{{"text", "string"}}};
constexpr std::array<std::string_view, 1> kClicked = {"clicked"};
constexpr std::array<WidgetKind, 2> kKinds = {{
    {"clock", "RmwClock", kClockProperties, {}},
    {"start", "RmwStart", {}, kClicked},
}};

TEST(BarSource, EveryItemGetsItsOwnSlot)
{
    BarConfig bar = bottomBar();
    bar.left = {{"start", {}}, {"clock", {}}};
    bar.right = {{"io.github.someone.weather", {}}, {"clock", {}}};
    const GeneratedBar generated = generateBarSource(bar, kKinds, "// widgets\n");
    ASSERT_EQ(generated.slots.size(), 3u);
    EXPECT_EQ(generated.slots[0].prefix, "w0");
    EXPECT_EQ(generated.slots[2].prefix, "w2");
    EXPECT_EQ(generated.slots[2].kind->id, "clock");
    EXPECT_EQ(generated.unknown, std::vector<std::string>{"io.github.someone.weather"});

    const std::string& source = generated.source;
    EXPECT_EQ(source.find("// widgets"), 0u);
    EXPECT_NE(source.find("in property <string> w1-text;"), std::string::npos);
    EXPECT_NE(source.find("in property <string> w2-text;"), std::string::npos);
    EXPECT_NE(source.find("callback w0-clicked();"), std::string::npos);
    EXPECT_NE(source.find("RmwStart { foreground: root.bar-foreground; vertical: false; clicked => { "
                          "root.w0-clicked(); } }"),
              std::string::npos);
    EXPECT_NE(source.find("HorizontalLayout"), std::string::npos);

    bar.position = BarPosition::Right;
    EXPECT_NE(generateBarSource(bar, kKinds, "").source.find("VerticalLayout"), std::string::npos);
}

} // namespace
