// Ricing Manager's Start menu (M11): its search, its place, the Win key — without windows.

#include "StartMenu.hpp"
#include "WinKeyHook.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace
{

using rmw::barapp::StartAnchor;
using rmw::barapp::WinKeyTracker;
using rmw::bar::Rect;

std::vector<rmw::platform::InstalledApp> apps()
{
    return {{"a", "Calculator"},    {"b", "Microsoft Edge"}, {"c", "Paint"},     {"d", "Calendário"},
            {"e", "Visual Studio"}, {"f", "Snipping Tool"},  {"g", "Notepad++"}, {"h", "Windows Terminal"}};
}

TEST(StartSearch, NamesThatStartWithItComeFirstThenWordsThenAnywhere)
{
    const auto list = apps();
    const auto names = [&](std::string_view query) {
        std::vector<std::string> out;
        for (const auto index : rmw::barapp::searchApps(list, query, 10))
        {
            out.push_back(list[index].name);
        }
        return out;
    };
    // "cal": Calculator and Calendário start with it (accents do not matter, case neither).
    EXPECT_EQ(names("CAL"), (std::vector<std::string>{"Calculator", "Calendário"}));
    EXPECT_EQ(names("calenda"), (std::vector<std::string>{"Calendário"}));
    // "term": a word of "Windows Terminal".
    EXPECT_EQ(names("term"), (std::vector<std::string>{"Windows Terminal"}));
    // "pa": Paint starts with it; "Notepad++" has it inside a word.
    EXPECT_EQ(names("pa"), (std::vector<std::string>{"Paint", "Notepad++"}));
    EXPECT_TRUE(names("").empty());
    EXPECT_TRUE(names("zzz").empty());
    EXPECT_EQ(rmw::barapp::searchApps(list, "i", 3).size(), 3u); // the limit
}

TEST(StartSearch, ThreeHundredAppsInUnder30Ms)
{
    std::vector<rmw::platform::InstalledApp> many;
    many.reserve(300);
    for (int i = 0; i < 300; ++i)
    {
        many.push_back({std::to_string(i), "Some Application Name Número " + std::to_string(i)});
    }
    const auto started = std::chrono::steady_clock::now();
    const auto found = rmw::barapp::searchApps(many, "número 29", 24);
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(found.size(), 11u); // 29, 290..299
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), 30);
}

TEST(StartPlace, NextToItsBarInsideTheWorkArea)
{
    StartAnchor anchor;
    anchor.work = {0, 0, 1920, 1032};
    anchor.bar = {0, 1032, 1920, 1080};
    anchor.edge = rmw::domain::BarPosition::Bottom;
    anchor.centered = true;
    // Windows 11's place: above the bar, in the middle.
    EXPECT_EQ(rmw::barapp::startMenuRect(anchor, 640, 700), (Rect{640, 324, 1280, 1024}));
    // A Start button at the start of a top bar: under it, on the left.
    anchor.edge = rmw::domain::BarPosition::Top;
    anchor.centered = false;
    anchor.bar = {0, 0, 1920, 36};
    anchor.work = {0, 36, 1920, 1080};
    EXPECT_EQ(rmw::barapp::startMenuRect(anchor, 640, 700), (Rect{8, 44, 648, 744}));
    // A left bar: beside it. Never past the work area, even when it is small.
    anchor.edge = rmw::domain::BarPosition::Left;
    anchor.bar = {0, 0, 52, 1080};
    anchor.work = {52, 0, 1920, 600};
    const Rect side = rmw::barapp::startMenuRect(anchor, 640, 700);
    EXPECT_EQ(side.left, 60);
    EXPECT_GE(side.top, 8);
    EXPECT_LE(side.bottom, 592);
}

TEST(StartWinKey, AloneOpensOursAnyShortcutStaysWindows)
{
    using enum WinKeyTracker::Verdict;
    WinKeyTracker tracker;
    // Win, alone.
    EXPECT_EQ(tracker.key(VK_LWIN, true), Pass);
    EXPECT_EQ(tracker.key(VK_LWIN, true), Pass); // auto-repeat
    EXPECT_EQ(tracker.key(VK_LWIN, false), OpenOurs);
    // Win+E, Win+L: Windows' shortcuts.
    EXPECT_EQ(tracker.key(VK_LWIN, true), Pass);
    EXPECT_EQ(tracker.key('E', true), Pass);
    EXPECT_EQ(tracker.key('E', false), Pass);
    EXPECT_EQ(tracker.key(VK_LWIN, false), Pass);
    EXPECT_EQ(tracker.key(VK_RWIN, true), Pass);
    EXPECT_EQ(tracker.key('L', true), Pass);
    EXPECT_EQ(tracker.key(VK_RWIN, false), Pass);
    // A key held before Win does not count; one let go while Win is down neither.
    EXPECT_EQ(tracker.key(VK_SHIFT, true), Pass);
    EXPECT_EQ(tracker.key(VK_LWIN, true), Pass);
    EXPECT_EQ(tracker.key(VK_SHIFT, false), Pass);
    EXPECT_EQ(tracker.key(VK_LWIN, false), OpenOurs);
    // A release without its press (the hook started while it was down): Windows'.
    EXPECT_EQ(tracker.key(VK_LWIN, false), Pass);
}

} // namespace
