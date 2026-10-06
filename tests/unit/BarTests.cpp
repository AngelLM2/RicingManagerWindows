#include "rmw/bar/Geometry.hpp"
#include "rmw/bar/Plan.hpp"
#include "rmw/bar/Source.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST(BarGeometry, AutoLengthIsWhatTheBarShows)
{
    BarConfig bar = bottomBar();
    bar.mode = rmw::domain::BarMode::Float;
    bar.length = {true, 100.0, true};
    bar.margin = {0, 0, 10, 0};
    const Rect work{0, 0, 1920, 1040};
    EXPECT_EQ(windowRect(bar, work, 1.0, 400.0), (Rect{760, 990, 1160, 1030})); // centered
    EXPECT_EQ(windowRect(bar, work, 1.5, 400.0).width(), 600);
    EXPECT_EQ(windowRect(bar, work, 1.0, 0.0).width(), 40);      // not yet known: as wide as thick
    EXPECT_EQ(windowRect(bar, work, 1.0, 5000.0).width(), 1920); // never past the edge
}

TEST(BarStyles, FillWhatTheBarAndItsSkinLeaveOut)
{
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(R"toml(
schema = 1
[meta]
name = "Styles"
[[bar]]
style     = "dock"
thickness = 70
[[bar]]
style = "status"
[[bar]]
length = "auto"
)toml",
                                             diagnostics);
    ASSERT_TRUE(rice.has_value()) << rice.error().message();
    ASSERT_EQ(rice->bars.size(), 3u);
    EXPECT_EQ(rice->bars[0].style, rmw::domain::BarStyle::Dock);
    EXPECT_TRUE(rice->bars[2].length.fit);

    const auto plan = planFromRice(*rice);
    ASSERT_TRUE(plan.has_value());
    const BarConfig& dock = plan->bars[0];
    EXPECT_EQ(dock.thickness, 70u); // the bar's own wins
    EXPECT_EQ(dock.position, BarPosition::Bottom);
    EXPECT_EQ(dock.mode, rmw::domain::BarMode::Reserve); // maximized windows end above it (v1.7)
    EXPECT_TRUE(dock.length.fit);
    EXPECT_TRUE(std::ranges::any_of(dock.center, [](const auto& item) { return item.id == "dock"; }));
    EXPECT_EQ(plan->bars[1].position, BarPosition::Top);
    EXPECT_EQ(plan->bars[1].thickness, 28u);
    // Through the worker's JSON, "auto" stays.
    const auto again = planFromJson(toJson(*plan));
    ASSERT_TRUE(again.has_value()) << again.error().message();
    EXPECT_TRUE(again->bars[0].length.fit);
    EXPECT_TRUE(again->bars[2].length.fit);

    // Written back as the rice said it: the style, the bar's own key, nothing else.
    const std::string written = rmw::domain::writeRice(*rice);
    EXPECT_NE(written.find("style = \"dock\"\nthickness = 70\n"), std::string::npos) << written;
    EXPECT_NE(written.find("length = \"auto\""), std::string::npos) << written;
    EXPECT_EQ(written.find("position = \"bottom\""), std::string::npos) << written;

    // Every style is a whole bar.
    for (const auto style : {rmw::domain::BarStyle::Windows, rmw::domain::BarStyle::Dock,
                             rmw::domain::BarStyle::Island, rmw::domain::BarStyle::Status,
                             rmw::domain::BarStyle::Side})
    {
        const BarConfig preset = rmw::domain::barStyle(style);
        EXPECT_GE(preset.left.size() + preset.center.size() + preset.right.size(), 4u) << toString(style);
        EXPECT_TRUE(preset.hideNativeTaskbar);
        EXPECT_GE(preset.keys.size(), 10u);
    }
    EXPECT_FALSE(rmw::domain::parseRice("schema = 1\n[meta]\nname = \"x\"\n[[bar]]\nstyle = \"neon\"\n", diagnostics)
                     .has_value());
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

TEST(BarPlan, CarriesTheRicesProgramIcons)
{
    rmw::domain::Rice rice;
    rice.icons.emplace();
    rice.icons->apps = {{"firefox.exe", "assets/icons/fox.ico"}};
    Plan plan;
    addAppIcons(plan, rice, R"(C:\rices\mine)");
    ASSERT_EQ(plan.appIcons.size(), 1u);
    EXPECT_EQ(plan.appIcons[0].file, std::filesystem::path(LR"(C:\rices\mine\assets\icons\fox.ico)"));
    const auto back = planFromJson(toJson(plan));
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    EXPECT_EQ(back->appIcons, plan.appIcons);

    for (const char* text : {
             R"({"version":1,"bars":[],"appIcons":[{"program":"firefox","file":"C:\\a.ico"}]})",
             R"({"version":1,"bars":[],"appIcons":[{"program":"firefox.exe","file":"a.ico"}]})",
             R"({"version":1,"bars":[],"appIcons":[{"program":"firefox.exe","file":"C:\\a.dll"}]})",
             R"({"version":1,"bars":[],"appIcons":{}})",
         })
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

TEST(BarSource, CallbacksPassTheirArgumentsThrough)
{
    static constexpr std::array<std::string_view, 2> kCallbacks = {"activate(int)", "moved(int, string)"};
    static constexpr std::array<WidgetKind, 1> kList = {
        {{"windows", "RmwWindows", {}, kCallbacks, "RmwWindowsColumn"}}};
    BarConfig bar = bottomBar();
    bar.center = {{"windows", {}}};
    const std::string source = generateBarSource(bar, kList, "").source;
    bar.position = BarPosition::Left;
    EXPECT_NE(generateBarSource(bar, kList, "").source.find("RmwWindowsColumn {"), std::string::npos);
    EXPECT_NE(source.find("callback w0-activate(int);"), std::string::npos);
    EXPECT_NE(source.find("callback w0-moved(int, string);"), std::string::npos);
    EXPECT_NE(source.find("activate(a0) => { root.w0-activate(a0); }"), std::string::npos);
    EXPECT_NE(source.find("moved(a0, a1) => { root.w0-moved(a0, a1); }"), std::string::npos);
}

} // namespace
