#include "rmw/domain/Settings.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace
{

using namespace rmw::domain;

constexpr std::array<std::string_view, 3> kActions = {"appearance.toggle", "rice.apply", "restore.original"};

rmw::Result<Settings> parse(std::string_view text, Diagnostics& diagnostics)
{
    return parseSettings(text, diagnostics, kActions);
}

TEST(KeyChord, ParsesModifiersAndKeys)
{
    const auto chord = KeyChord::parse("Ctrl+Win+Alt+D");
    ASSERT_TRUE(chord.has_value());
    EXPECT_EQ(chord->modifiers, 0x2u | 0x8u | 0x1u);
    EXPECT_EQ(chord->virtualKey, static_cast<std::uint32_t>('D'));
    EXPECT_EQ(chord->toString(), "Ctrl+Alt+Win+D");

    EXPECT_EQ(KeyChord::parse("win + alt + f12")->virtualKey, 0x7Bu);
    EXPECT_EQ(KeyChord::parse("Ctrl+Shift+Numpad7")->virtualKey, 0x67u);
    EXPECT_EQ(KeyChord::parse("Alt+PageDown")->virtualKey, 0x22u);
    EXPECT_EQ(KeyChord::parse("F13")->modifiers, 0u); // extended function keys may stand alone
}

TEST(KeyChord, RejectsUnsafeOrMalformedChords)
{
    for (const char* text :
         {"", "D", "Shift+D", "Ctrl+", "Ctrl+Ctrl+D", "Ctrl+D+E", "Ctrl+Hyper", "Alt+F25", "Win"})
    {
        EXPECT_FALSE(KeyChord::parse(text).has_value()) << text;
    }
}

TEST(Settings, ReadsHotkeysRulesAndTray)
{
    Diagnostics diagnostics;
    const auto settings = parse(R"toml(
schema = 1

[core]
keep_in_tray = true

[[hotkey]]
keys = "Win+Alt+D"
action = "appearance.toggle"

[[hotkey]]
keys = "Ctrl+Win+Alt+N"
action = "rice.apply"
rice = 'C:\Rices\night'

[[rule]]
when = "time"
at = "19:30"
action = "rice.apply"
rice = 'C:\Rices\night'

[[rule]]
when = "battery"
action = "appearance.toggle"

[[rule]]
when = "monitor-connected"
monitor = "edid:DEL40F7-UID256"
action = "restore.original"
)toml",
                                diagnostics);
    ASSERT_TRUE(settings.has_value()) << settings.error().toString();
    EXPECT_TRUE(diagnostics.all().empty());
    EXPECT_TRUE(settings->keepInTray);
    ASSERT_EQ(settings->hotkeys.size(), 2u);
    EXPECT_EQ(settings->hotkeys[1].action.ricePath, R"(C:\Rices\night)");
    ASSERT_EQ(settings->rules.size(), 3u);
    EXPECT_EQ(settings->rules[0].at, (TimeOfDay{19, 30}));
    EXPECT_EQ(settings->rules[1].trigger, RuleTrigger::OnBattery);
    EXPECT_EQ(settings->rules[2].monitor->edidKey, "DEL40F7-UID256");
    EXPECT_TRUE(settings->needsResidency());

    Diagnostics empty;
    EXPECT_FALSE(parse("", empty)->needsResidency());
}

TEST(Settings, ReportsMistakesWithLocations)
{
    const char* bad[] = {
        "[[hotkey]]\nkeys = \"Win+Alt+D\"\naction = \"launch.rocket\"\n", // unknown action
        "[[hotkey]]\nkeys = \"D\"\naction = \"appearance.toggle\"\n",     // no modifier
        "[[hotkey]]\naction = \"appearance.toggle\"\n",                   // no keys
        "[[hotkey]]\nkeys = \"Win+Alt+D\"\naction = \"rice.apply\"\n",    // rice.apply without rice
        "[[hotkey]]\nkeys = \"Win+Alt+D\"\naction = \"appearance.toggle\"\n"
        "[[hotkey]]\nkeys = \"Alt+Win+D\"\naction = \"restore.original\"\n",       // same chord twice
        "[[rule]]\nwhen = \"time\"\naction = \"appearance.toggle\"\n",             // time without at
        "[[rule]]\nwhen = \"sunrise\"\naction = \"appearance.toggle\"\n",          // unknown trigger
        "[[rule]]\nwhen = \"monitor-connected\"\naction = \"restore.original\"\n", // no monitor
        "schema = 2\n",
    };
    for (const char* text : bad)
    {
        Diagnostics diagnostics;
        EXPECT_FALSE(parse(text, diagnostics).has_value()) << text;
        EXPECT_TRUE(diagnostics.hasErrors()) << text;
    }

    Diagnostics diagnostics;
    const auto located = parse("\n\n[[hotkey]]\nkeys = \"Win+Alt+Q\"\naction = \"nope\"\n", diagnostics);
    ASSERT_FALSE(located.has_value());
    EXPECT_EQ(diagnostics.all()[0].line, 5u);
    EXPECT_EQ(diagnostics.all()[0].key, "hotkey[0].action");
}

} // namespace
