#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/tiling/Desktop.hpp"
#include "rmw/tiling/Layout.hpp"
#include "rmw/tiling/Plan.hpp"
#include "rmw/tiling/Workspace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace rmw::tiling;
using rmw::domain::TilingCommand;

const Rect kArea{0, 0, 1920, 1040};
const LayoutOptions kNoGaps{0, 0, 0.5};

bool overlap(const Rect& a, const Rect& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

bool has(const std::vector<WindowId>& list, WindowId window)
{
    return std::ranges::find(list, window) != list.end();
}

TEST(TilingLayout, DwindleHalvesAndTurns)
{
    const auto one = arrange(LayoutKind::Dwindle, kArea, 1, kNoGaps);
    EXPECT_EQ(one, (std::vector<Rect>{kArea}));
    const auto three = arrange(LayoutKind::Dwindle, kArea, 3, kNoGaps);
    ASSERT_EQ(three.size(), 3u);
    EXPECT_EQ(three[0], (Rect{0, 0, 960, 1040}));   // the left half
    EXPECT_EQ(three[1], (Rect{960, 0, 1920, 520})); // the right half, cut across
    EXPECT_EQ(three[2], (Rect{960, 520, 1920, 1040}));
}

TEST(TilingLayout, GapsAtTheEdgesAndBetween)
{
    const LayoutOptions gaps{10, 6, 0.5};
    const auto two = arrange(LayoutKind::Columns, kArea, 2, gaps);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0].left, 10);
    EXPECT_EQ(two[0].top, 10);
    EXPECT_EQ(two[1].right, 1910);
    EXPECT_EQ(two[1].bottom, 1030);
    EXPECT_EQ(two[1].left - two[0].right, 6);
    EXPECT_EQ(two[0].width() + two[1].width(), 1900 - 6); // the room, shared to the pixel
}

TEST(TilingLayout, MasterStackAndColumnsAndMonocle)
{
    const LayoutOptions options{0, 0, 0.6};
    const auto stack = arrange(LayoutKind::MasterStack, kArea, 4, options);
    ASSERT_EQ(stack.size(), 4u);
    EXPECT_EQ(stack[0], (Rect{0, 0, 1152, 1040})); // 60%
    EXPECT_EQ(stack[1].left, 1152);
    EXPECT_EQ(stack[3].bottom, 1040);
    EXPECT_EQ(stack[1].height() + stack[2].height() + stack[3].height(), 1040);

    const auto columns = arrange(LayoutKind::Columns, kArea, 3, kNoGaps);
    EXPECT_EQ(columns[0].width() + columns[1].width() + columns[2].width(), 1920);

    const auto monocle = arrange(LayoutKind::Monocle, kArea, 3, kNoGaps);
    EXPECT_TRUE(std::ranges::all_of(monocle, [](const Rect& r) { return r == kArea; }));
    EXPECT_TRUE(arrange(LayoutKind::Floating, kArea, 3, kNoGaps).empty());
    EXPECT_TRUE(arrange(LayoutKind::Dwindle, kArea, 0, kNoGaps).empty());
    EXPECT_TRUE(arrange(LayoutKind::Dwindle, Rect{5, 5, 5, 900}, 2, kNoGaps).empty()); // no room at all
}

TEST(TilingLayout, ManyWindowsNeverOverlapNorLeaveTheArea)
{
    const LayoutOptions gaps{8, 8, 0.55};
    for (const auto layout : {LayoutKind::Dwindle, LayoutKind::MasterStack, LayoutKind::Columns})
    {
        for (std::size_t count = 1; count <= 40; ++count)
        {
            const auto rects = arrange(layout, kArea, count, gaps);
            ASSERT_EQ(rects.size(), count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::string where{rmw::domain::toString(layout)};
                EXPECT_FALSE(rects[i].empty()) << where << " " << count;
                EXPECT_GE(rects[i].left, kArea.left);
                EXPECT_LE(rects[i].right, kArea.right);
                EXPECT_GE(rects[i].top, kArea.top);
                EXPECT_LE(rects[i].bottom, kArea.bottom);
                for (std::size_t j = i + 1; j < count; ++j)
                {
                    EXPECT_FALSE(overlap(rects[i], rects[j])) << where << " " << count << ": " << i << " " << j;
                }
            }
        }
    }
    // A tiny area: still something for everyone.
    EXPECT_EQ(arrange(LayoutKind::Dwindle, Rect{0, 0, 10, 10}, 6, gaps).size(), 6u);
}

TEST(TilingLayout, WindowsThatKeepAMinimumGetRoom)
{
    // Columns: the middle one keeps 1000 px, the others share the rest.
    const std::vector<Size> middle = {{}, {1000, 0}, {}};
    const auto columns = arrange(LayoutKind::Columns, kArea, 3, kNoGaps, middle);
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_EQ(columns[1].width(), 1000);
    EXPECT_EQ(columns[0].width(), 460);
    EXPECT_EQ(columns[2].right, 1920);

    // Master and stack: a stack that needs 1200 px narrows the master (60% would leave it 768).
    const std::vector<Size> wideStack = {{}, {1200, 0}, {}};
    const auto stack = arrange(LayoutKind::MasterStack, kArea, 3, LayoutOptions{0, 0, 0.6}, wideStack);
    ASSERT_EQ(stack.size(), 3u);
    EXPECT_EQ(stack[0].width(), 720);
    EXPECT_EQ(stack[1].width(), 1200);

    // Dwindle: the last one needs 700 px of height; the one above it gives them up.
    const std::vector<Size> tall = {{}, {}, {0, 700}};
    const auto dwindle = arrange(LayoutKind::Dwindle, kArea, 3, kNoGaps, tall);
    ASSERT_EQ(dwindle.size(), 3u);
    EXPECT_EQ(dwindle[2].height(), 700);
    EXPECT_EQ(dwindle[1].height(), 340);
    EXPECT_FALSE(overlap(dwindle[1], dwindle[2]));

    // Two that cannot both fit: shared in proportion, never more than the room, never overlapping.
    const std::vector<Size> both = {{1200, 0}, {1200, 0}};
    const auto shared = arrange(LayoutKind::Columns, kArea, 2, kNoGaps, both);
    ASSERT_EQ(shared.size(), 2u);
    EXPECT_EQ(shared[0].width(), 960);
    EXPECT_EQ(shared[1].right, 1920);

    // No minimum known: exactly as before.
    EXPECT_EQ(arrange(LayoutKind::Dwindle, kArea, 5, kNoGaps, std::vector<Size>(5)),
              arrange(LayoutKind::Dwindle, kArea, 5, kNoGaps));
}

TEST(TilingLayout, AWindowLargerThanItsSlotStaysOnTheScreen)
{
    // It grows from its slot, and back inside the area when that passes the edge.
    EXPECT_EQ(fitted(Rect{1500, 600, 1900, 1000}, Size{600, 500}, kArea), (Rect{1320, 540, 1920, 1040}));
    EXPECT_EQ(fitted(Rect{10, 10, 400, 300}, Size{200, 100}, kArea), (Rect{10, 10, 400, 300})); // it fits
    EXPECT_EQ(fitted(Rect{100, 100, 500, 500}, Size{2500, 0}, kArea).left, 0); // wider than the screen
}

TEST(TilingLayout, AWindowSlidesIntoPlace)
{
    const Rect from{2000, 0, 2960, 1040}; // off to the right: a workspace coming in
    const Rect to{960, 0, 1920, 1040};
    EXPECT_EQ(slideFrame(from, to, 0.0), from);
    EXPECT_EQ(slideFrame(from, to, 1.0), to);
    EXPECT_EQ(slideFrame(from, to, 7.5), to);
    // Fast first, then slowing: past the middle at half the time (ease-out).
    const Rect half = slideFrame(from, to, 0.5);
    EXPECT_LT(half.left, (from.left + to.left) / 2);
    EXPECT_GT(half.left, to.left);
    EXPECT_EQ(half.width(), 960);
    // Growing: the old size on the way, the new one at the end; shrinking: the new one at once.
    EXPECT_EQ(slideFrame(Rect{0, 0, 400, 300}, Rect{100, 0, 1100, 800}, 0.3).width(), 400);
    EXPECT_EQ(slideFrame(Rect{0, 0, 1000, 800}, Rect{500, 0, 900, 300}, 0.01).height(), 300);
}

TEST(TilingLayout, NeighborsByDirection)
{
    // The three of DwindleHalvesAndTurns: left half, top right, bottom right.
    const auto rects = arrange(LayoutKind::Dwindle, kArea, 3, kNoGaps);
    EXPECT_EQ(neighbor(rects, 0, Direction::Right), 1u); // the nearer across
    EXPECT_EQ(neighbor(rects, 1, Direction::Down), 2u);
    EXPECT_EQ(neighbor(rects, 2, Direction::Up), 1u);
    EXPECT_EQ(neighbor(rects, 2, Direction::Left), 0u);
    EXPECT_FALSE(neighbor(rects, 0, Direction::Left).has_value());
    EXPECT_FALSE(neighbor(rects, 1, Direction::Up).has_value());
    EXPECT_FALSE(neighbor(rects, 9, Direction::Up).has_value());
}

TEST(TilingLayout, TheNextLayoutSkipsFloating)
{
    EXPECT_EQ(nextLayout(LayoutKind::Dwindle), LayoutKind::MasterStack);
    EXPECT_EQ(nextLayout(LayoutKind::Columns), LayoutKind::Monocle);
    EXPECT_EQ(nextLayout(LayoutKind::Monocle), LayoutKind::Dwindle);
    EXPECT_EQ(nextLayout(LayoutKind::Floating), LayoutKind::Dwindle);
}

TEST(TilingWorkspace, WindowsComeGoFloatAndTradePlaces)
{
    Workspace workspace;
    workspace.add(1);
    workspace.add(2);
    workspace.add(3);
    EXPECT_EQ(workspace.focused(), 3u);
    EXPECT_EQ(workspace.tiled(), (std::vector<WindowId>{1, 2, 3}));
    // Floating: out of the layout, still known.
    workspace.toggleFloating(2);
    EXPECT_EQ(workspace.tiled(), (std::vector<WindowId>{1, 3}));
    EXPECT_EQ(workspace.windows().size(), 3u);
    EXPECT_EQ(workspace.placements(kArea, kNoGaps).size(), 2u);
    workspace.toggleFloating(2);
    // The first window and its right neighbor trade places.
    EXPECT_TRUE(workspace.exchange(1, Direction::Right, kArea, kNoGaps));
    EXPECT_EQ(workspace.tiled().front(), 2u);
    EXPECT_FALSE(workspace.exchange(2, Direction::Left, kArea, kNoGaps));
    EXPECT_TRUE(workspace.exchange(2, 3)); // dragged onto another
    EXPECT_EQ(workspace.tiled().front(), 3u);
    EXPECT_FALSE(workspace.exchange(3, 99));
    workspace.promote(2);
    EXPECT_EQ(workspace.tiled().front(), 2u);
    // A closed window: the newest left gets the focus.
    workspace.focus(3);
    EXPECT_TRUE(workspace.remove(3));
    EXPECT_FALSE(workspace.remove(3));
    EXPECT_EQ(workspace.focused(), workspace.tiled().back());
    EXPECT_FALSE(workspace.contains(3));
}

TEST(TilingWorkspace, MonocleGoesBackToWhatItWas)
{
    Workspace workspace(LayoutKind::MasterStack);
    workspace.add(1);
    workspace.add(2);
    workspace.toggleMonocle();
    EXPECT_EQ(workspace.layout(), LayoutKind::Monocle);
    // In monocle, left and right go through the windows in order.
    EXPECT_EQ(workspace.neighborOf(1, Direction::Right, kArea, kNoGaps), 2u);
    EXPECT_FALSE(workspace.neighborOf(1, Direction::Left, kArea, kNoGaps).has_value());
    workspace.toggleMonocle();
    EXPECT_EQ(workspace.layout(), LayoutKind::MasterStack);
    workspace.cycleLayout();
    EXPECT_EQ(workspace.layout(), LayoutKind::Columns);
}

std::vector<Monitor> twoMonitors()
{
    return {Monitor{R"(\\.\DISPLAY1)", kArea, Rect{0, 0, 1920, 1080}, 1.0},
            Monitor{R"(\\.\DISPLAY2)", Rect{1920, 0, 3200, 1024}, Rect{1920, 0, 3200, 1024}, 1.0}};
}

TEST(TilingDesktop, EachMonitorShowsOneOfItsWorkspaces)
{
    Desktop desktop(4, LayoutKind::Dwindle);
    EXPECT_TRUE(desktop.setMonitors(twoMonitors()).show.empty());
    EXPECT_EQ(desktop.add(1, 0).focus, 1u);
    desktop.add(2, 0);
    desktop.add(3, 1);
    // A rule's workspace that is not shown: the window hides at once.
    const auto ruled = desktop.add(4, 0, 2);
    EXPECT_EQ(ruled.hide, std::vector<WindowId>{4});
    EXPECT_FALSE(desktop.shown(4));
    EXPECT_EQ(desktop.occupied(0), 0b101u);
    EXPECT_EQ(desktop.placements(kNoGaps).size(), 3u); // 1, 2 and 3: the shown ones

    // Workspace 3 on the first monitor: 1 and 2 hide, 4 shows and takes the focus; the second
    // monitor does not change.
    const auto go = desktop.goTo(0, 2);
    EXPECT_EQ(go.hide, (std::vector<WindowId>{1, 2}));
    EXPECT_EQ(go.show, std::vector<WindowId>{4});
    EXPECT_EQ(go.focus, 4u);
    EXPECT_TRUE(desktop.shown(3));
    EXPECT_EQ(desktop.active(0), 2);
    // The same one again: nothing to hide or show.
    EXPECT_TRUE(desktop.goTo(0, 2).hide.empty());
    EXPECT_TRUE(desktop.goTo(0, 9).show.empty()); // out of range

    // Send the focused window to workspace 1 (not shown): it hides.
    const auto sent = desktop.sendTo(4, 0);
    EXPECT_EQ(sent.hide, std::vector<WindowId>{4});
    EXPECT_EQ(desktop.find(4), (Place{0, 0}));
    // Across monitors: into the other one's shown workspace.
    const auto moved = desktop.moveToMonitor(3, 0);
    EXPECT_EQ(moved.focus, 3u);
    EXPECT_EQ(desktop.find(3), (Place{0, 2}));
    EXPECT_EQ(desktop.monitorToward(0, Direction::Right), 1u);
    EXPECT_FALSE(desktop.monitorToward(0, Direction::Left).has_value());

    EXPECT_TRUE(desktop.remove(1));
    EXPECT_FALSE(desktop.remove(1));
    EXPECT_EQ(desktop.all().size(), 3u);
}

TEST(TilingDesktop, MonitorsAndWorkspacesThatGoAway)
{
    Desktop desktop(4, LayoutKind::Columns);
    desktop.setMonitors(twoMonitors());
    desktop.add(1, 1);
    desktop.add(2, 1, 3); // hidden on the second monitor's last workspace
    // The second monitor is unplugged: what it showed comes to what the first shows; its other workspaces
    // join the first's of the same number (not all piled on the screen).
    auto monitors = twoMonitors();
    monitors.pop_back();
    const auto unplugged = desktop.setMonitors(monitors);
    EXPECT_FALSE(has(unplugged.show, 1)); // it was on screen already
    EXPECT_FALSE(has(unplugged.show, 2));
    EXPECT_EQ(desktop.monitorCount(), 1u);
    EXPECT_TRUE(desktop.shown(1));
    EXPECT_EQ(desktop.find(2), (Place{0, 3}));
    desktop.goTo(0, 0);

    // A monitor whose name changed (a DisplayPort one waking up) is the one in the same place.
    Desktop renamed(4, LayoutKind::Columns);
    renamed.setMonitors(twoMonitors());
    renamed.add(7, 1, 2);
    renamed.goTo(1, 2);
    auto newNames = twoMonitors();
    newNames[1].key = R"(\\.\DISPLAY9)";
    EXPECT_TRUE(renamed.setMonitors(newNames).show.empty());
    EXPECT_EQ(renamed.find(7), (Place{1, 2}));
    EXPECT_EQ(renamed.active(1), 2);

    // The first monitor keeps its workspaces when another one comes.
    desktop.goTo(0, 3);
    desktop.add(3, 0);
    desktop.setMonitors(twoMonitors());
    EXPECT_EQ(desktop.active(0), 3);
    // Two workspaces only: the windows of 3 and 4 join 2, which is now shown.
    const auto fewer = desktop.setWorkspaceCount(2);
    EXPECT_EQ(desktop.workspaceCount(), 2);
    EXPECT_EQ(desktop.active(0), 1);
    EXPECT_EQ(desktop.find(3), (Place{0, 1}));
    EXPECT_TRUE(fewer.hide.empty());
    desktop.setLayout(LayoutKind::Monocle);
    EXPECT_EQ(desktop.activeWorkspace(0).layout(), LayoutKind::Monocle);
    EXPECT_TRUE(desktop.setMonitors({}).show.empty()); // no monitors: nothing changes
    EXPECT_EQ(desktop.monitorCount(), 2u);
}

Plan samplePlan()
{
    Plan plan;
    plan.settings.enabled = true;
    plan.settings.mode = rmw::domain::TilingMode::Mac;
    plan.settings.layout = LayoutKind::MasterStack;
    plan.settings.gap = 4;
    plan.settings.rules.push_back({"Spotify.exe", "", "", rmw::domain::TilingRuleAction::Workspace, 3});
    plan.settings.rules.push_back({"", "Chrome_WidgetWin_1", "picture", rmw::domain::TilingRuleAction::Float, 1});
    plan.border = rmw::domain::Color{0xFF, 0x79, 0xC6, 0xFF};
    return plan;
}

TEST(TilingPlan, GoesThereAndBack)
{
    const Plan plan = samplePlan();
    const auto again = parsePlan(toJson(plan));
    ASSERT_TRUE(again.has_value()) << again.error().toString();
    EXPECT_EQ(*again, plan);

    for (const char* bad : {"", "[]", "{\"version\": 2}", "{\"version\": 1}"})
    {
        EXPECT_FALSE(parsePlan(bad).has_value()) << bad;
    }
    auto text = toJson(plan);
    text.replace(text.find("\"masterSize\": 55"), 16, "\"masterSize\": 99");
    EXPECT_FALSE(parsePlan(text).has_value());

    const auto options = layoutOptions(plan.settings, 1.5);
    EXPECT_EQ(options.innerGap, 6);
    EXPECT_EQ(options.outerGap, 12);
    EXPECT_DOUBLE_EQ(options.masterRatio, 0.55);
}

TEST(TilingPlan, TheRicesWindowLook)
{
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(R"toml(
schema = 1
[meta]
name = "Look"

[palette]
night = "#1a1b26"

[windows]
corners = "small"
border = "none"
caption = "palette.night"
title = "#c0caf5"
backdrop = "mica"
dark = true
exclude = ["Code.exe"]
)toml",
                                             diagnostics);
    ASSERT_TRUE(rice.has_value()) << diagnostics.errorSummary("rice.toml");
    ASSERT_TRUE(rice->windows.has_value());
    EXPECT_TRUE(rice->windows->noBorder);
    EXPECT_EQ(rice->windows->exclude, std::vector<std::string>{"code.exe"});
    const auto again = rmw::domain::parseRice(rmw::domain::writeRice(*rice), diagnostics);
    ASSERT_TRUE(again.has_value()) << rmw::domain::writeRice(*rice);
    EXPECT_EQ(again->windows->caption, rice->windows->caption);
    EXPECT_TRUE(again->windows->noBorder);
    EXPECT_EQ(again->windows->exclude, rice->windows->exclude);

    const WindowLook look = lookFromRice(*rice->windows, rice->palette, rmw::domain::Color{0, 0x78, 0xD4, 0xFF});
    EXPECT_EQ(look.corners, rmw::domain::WindowCorners::Small);
    EXPECT_EQ(look.caption, (rmw::domain::Color{0x1a, 0x1b, 0x26, 0xFF})); // the palette's
    EXPECT_EQ(look.backdrop, rmw::domain::WindowBackdrop::Mica);
    EXPECT_TRUE(look.noBorder);

    // In the plan, with tiling or alone; a plan of before M14 tiles.
    Plan plan = samplePlan();
    plan.look = look;
    plan.settings.enabled = false;
    const auto back = parsePlan(toJson(plan));
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    EXPECT_EQ(*back, plan);
    EXPECT_EQ(parseLook(toJson(look)).value(), look);
    auto old = toJson(samplePlan());
    old.erase(old.find("\"tiling\": true,"), 15);
    ASSERT_TRUE(parsePlan(old).has_value()) << old;
    EXPECT_TRUE(parsePlan(old)->settings.enabled);

    for (const char* bad : {
             "[windows]\ncorners = \"oval\"\n",
             "[windows]\nbackdrop = \"glass\"\n",
             "[windows]\ncaption = \"palette.nowhere\"\n", // no such palette color
             "[windows]\nexclude = [\"code\"]\n",
         })
    {
        rmw::domain::Diagnostics d;
        EXPECT_FALSE(rmw::domain::parseRice(std::string("schema = 1\n[meta]\nname = \"x\"\n") + bad, d).has_value())
            << bad;
    }
    EXPECT_FALSE(parseLook(R"({"corners": "oval"})").has_value());
    EXPECT_FALSE(parseLook(R"({"exclude": ["../x.exe"]})").has_value());
}

TEST(TilingPlan, ShortcutsAndRulesMatch)
{
    const auto keys = rmw::domain::defaultTilingKeys();
    constexpr std::uint32_t kCtrlAlt = 0x3;
    constexpr std::uint32_t kCtrlWin = 0xA;
    EXPECT_EQ(matchKey(keys, kCtrlAlt, 0x25), (KeyMatch{TilingCommand::FocusLeft, 0}));
    EXPECT_EQ(matchKey(keys, kCtrlWin, '4'), (KeyMatch{TilingCommand::Workspace, 4}));
    EXPECT_EQ(matchKey(keys, kCtrlWin, 0x67), (KeyMatch{TilingCommand::Workspace, 7})); // numpad 7
    EXPECT_EQ(matchKey(keys, kCtrlWin | 0x4, '2'), (KeyMatch{TilingCommand::SendToWorkspace, 2}));
    EXPECT_FALSE(matchKey(keys, kCtrlWin, '0').has_value());
    EXPECT_FALSE(matchKey(keys, kCtrlAlt | 0x4, 'F').has_value()); // more modifiers: not Float
    EXPECT_FALSE(matchKey(keys, 0, 0x25).has_value());

    const Plan plan = samplePlan();
    const auto* spotify = matchRule(plan.settings.rules, {"spotify.EXE", "Chrome_WidgetWin_0", "Spotify Free"});
    ASSERT_NE(spotify, nullptr);
    EXPECT_EQ(spotify->workspace, 3);
    EXPECT_NE(matchRule(plan.settings.rules, {"chrome.exe", "Chrome_WidgetWin_1", "Picture in Picture"}), nullptr);
    EXPECT_EQ(matchRule(plan.settings.rules, {"chrome.exe", "Chrome_WidgetWin_1", "New tab"}), nullptr);
    // Window classes regardless of case, as Windows tells them apart.
    EXPECT_NE(matchRule(plan.settings.rules, {"chrome.exe", "chrome_widgetwin_1", "Picture in Picture"}), nullptr);
}

TEST(TilingPlan, TheWindowsToPutBack)
{
    const std::vector<Original> windows = {
        {0x1A2B3C, 4242, Rect{-1900, 10, -100, 900}, false, true},
        {0x10, 8, Rect{0, 0, 800, 600}, true, false, false},
    };
    const auto again = parseOriginals(toJson(windows));
    ASSERT_TRUE(again.has_value()) << again.error().toString();
    EXPECT_EQ(*again, windows);
    EXPECT_TRUE(parseOriginals(R"({"version": 1, "windows": []})")->empty());
    for (const char* bad : {
             R"({"version": 1, "windows": [{"window": 1, "process": 2, "rect": [0, 0, 1], "maximized": false, "hidden": false, "placed": true}]})",
             R"({"version": 1, "windows": [{"window": 0, "process": 2, "rect": [0, 0, 1, 1], "maximized": false, "hidden": false, "placed": true}]})",
             R"({"version": 1, "windows": [{"window": 1, "process": 2, "rect": [0, 0, 1, 99999999], "maximized": false, "hidden": false, "placed": true}]})",
             R"({"windows": []})",
         })
    {
        EXPECT_FALSE(parseOriginals(bad).has_value()) << bad;
    }
}

} // namespace
