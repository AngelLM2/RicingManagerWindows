// The lifecycle review (v1.8.1): what starts the core, and what is put back when a worker ends badly.
#include "CoreNeed.hpp"

#include "rmw/platform/Keyboard.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{

using rmw::cli::CoreNeed;
using rmw::cli::coreNeed;
using rmw::platform::KeyStroke;
using rmw::platform::kMaskKey;
using rmw::platform::modifierReleases;

constexpr std::uint16_t kLeftShift = 0xA0;
constexpr std::uint16_t kRightControl = 0xA3;
constexpr std::uint16_t kLeftAlt = 0xA4;
constexpr std::uint16_t kLeftWin = 0x5B;

// The installer runs `rmw shutdown --wait` and the uninstaller `rmw autostart off` too: with nothing
// running, they started a whole core (the bar, the tiler, maybe a UAC prompt) only to end it.
TEST(CliCoreNeed, StoppingOrTheStartEntryNeverStartsACore)
{
    EXPECT_EQ(coreNeed("shutdown"), CoreNeed::IfRunning);
    EXPECT_EQ(coreNeed("autostart"), CoreNeed::None);
    EXPECT_EQ(coreNeed("pack"), CoreNeed::None);
    EXPECT_EQ(coreNeed("validate"), CoreNeed::None);
    for (const char* command : {"apply", "status", "restore", "action", "settings", "history", "monitors"})
    {
        EXPECT_EQ(coreNeed(command), CoreNeed::Start) << command;
    }
}

TEST(StuckModifiers, NothingDownNothingSent)
{
    EXPECT_TRUE(modifierReleases([](std::uint16_t) { return false; }).empty());
}

TEST(StuckModifiers, EachOneDownIsLetGo)
{
    const auto strokes =
        modifierReleases([](std::uint16_t vk) { return vk == kLeftShift || vk == kRightControl; });
    EXPECT_EQ(strokes, (std::vector<KeyStroke>{{kLeftShift, true}, {kRightControl, true}}));
}

// Win or Alt let go alone opens Start or a menu bar: a key nobody uses comes first.
TEST(StuckModifiers, WinOrAltGoAfterAMaskKey)
{
    for (const std::uint16_t key : {kLeftWin, kLeftAlt})
    {
        const auto strokes = modifierReleases([key](std::uint16_t vk) { return vk == key; });
        EXPECT_EQ(strokes, (std::vector<KeyStroke>{{kMaskKey, false}, {kMaskKey, true}, {key, true}})) << key;
    }
    const auto all = modifierReleases([](std::uint16_t) { return true; });
    EXPECT_EQ(all.size(), rmw::platform::kModifierKeys.size() + 2);
    EXPECT_TRUE(std::ranges::all_of(all.begin() + 2, all.end(), [](const KeyStroke& s) { return s.up; }));
}

} // namespace
