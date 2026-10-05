#include "rmw/domain/MonitorSelector.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using namespace rmw::domain;
using Kind = MonitorSelector::Kind;

MonitorSelector sel(const char* text)
{
    return MonitorSelector::parse(text).value();
}

// Three monitors: left (1080p at -1920), primary in the middle, a portrait one on the right.
std::vector<MonitorFacts> threeMonitors()
{
    return {
        {"edid:B", true, 0, 0, 2560, 1440},
        {"edid:A", false, -1920, 200, 0, 1280},
        {"edid:C", false, 2560, -400, 3640, 1520},
    };
}

TEST(MonitorSelector, ParsesEverySelector)
{
    EXPECT_EQ(sel("primary").kind, Kind::Primary);
    EXPECT_EQ(sel("all").kind, Kind::All);
    EXPECT_EQ(sel("bottom").kind, Kind::Bottom);
    const auto edid = sel("edid:JRY-2380-1");
    EXPECT_EQ(edid.kind, Kind::Edid);
    EXPECT_EQ(edid.edidKey, "JRY-2380-1");
    EXPECT_EQ(edid.toString(), "edid:JRY-2380-1");
    for (const char* bad : {"", "Primary", "edid:", "edid:a b", "monitor2", "edid:\x01"})
    {
        EXPECT_FALSE(MonitorSelector::parse(bad).has_value()) << bad;
    }
}

TEST(MonitorSelector, SpecificRulesWinOverAllWhereverAllAppears)
{
    const std::vector<MonitorSelector> rules = {sel("all"), sel("primary")};
    EXPECT_EQ(resolveMonitorRules(rules, threeMonitors()), (std::vector<int>{1, 0, 0}));
}

TEST(MonitorSelector, FirstMatchingRuleWins)
{
    const std::vector<MonitorSelector> rules = {sel("left"), sel("edid:A"), sel("right")};
    EXPECT_EQ(resolveMonitorRules(rules, threeMonitors()), (std::vector<int>{-1, 0, 2}));
}

TEST(MonitorSelector, PositionalSelectorsPickTheExtremes)
{
    const auto monitors = threeMonitors();
    const std::vector<MonitorSelector> top = {sel("top")};
    const std::vector<MonitorSelector> bottom = {sel("bottom")};
    EXPECT_EQ(resolveMonitorRules(top, monitors), (std::vector<int>{-1, -1, 0}));
    EXPECT_EQ(resolveMonitorRules(bottom, monitors), (std::vector<int>{-1, -1, 0}));
}

TEST(MonitorSelector, TiesPreferThePrimaryMonitor)
{
    const std::vector<MonitorFacts> stacked = {
        {"x", false, 0, -1080, 1920, 0},
        {"y", true, 0, 0, 1920, 1080},
    };
    const std::vector<MonitorSelector> left = {sel("left")};
    // Same left edge: the upper one wins on the secondary axis.
    EXPECT_EQ(resolveMonitorRules(left, stacked), (std::vector<int>{0, -1}));
    const std::vector<MonitorFacts> identical = {{"x", false, 0, 0, 10, 10}, {"y", true, 0, 0, 10, 10}};
    EXPECT_EQ(resolveMonitorRules(left, identical), (std::vector<int>{-1, 0}));
}

TEST(MonitorSelector, UnmatchedMonitorsKeepWhatTheyHave)
{
    const std::vector<MonitorSelector> rules = {sel("edid:missing")};
    EXPECT_EQ(resolveMonitorRules(rules, threeMonitors()), (std::vector<int>{-1, -1, -1}));
    EXPECT_TRUE(resolveMonitorRules(rules, {}).empty());
}

} // namespace
