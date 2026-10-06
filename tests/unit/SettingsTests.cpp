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

    // Since v1.1 the tray icon stays unless the user turns it off ("zero resident").
    Diagnostics empty;
    EXPECT_TRUE(parse("", empty)->needsResidency());
    EXPECT_FALSE(parse("[core]\nkeep_in_tray = false\n", empty)->needsResidency());
}

TEST(Settings, ReportsMistakesWithLocations)
{
    const char* bad[] = {
        "[[hotkey]]\nkeys = \"Win+Alt+D\"\naction = \"launch.rocket\"\n",          // unknown action
        "[[hotkey]]\nkeys = \"D\"\naction = \"appearance.toggle\"\n",              // no modifier
        "[[hotkey]]\naction = \"appearance.toggle\"\n",                            // no keys
        "[[hotkey]]\nkeys = \"Win+Alt+D\"\naction = \"rice.apply\"\n",             // rice.apply without rice
        R"([[hotkey]]
keys = "Win+Alt+D"
action = "appearance.toggle"
[[hotkey]]
keys = "Alt+Win+D"
action = "restore.original"
)",                                                                                // same chord twice
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

TEST(TilingSettings, ReadsEverythingAndKeepsTheDefaultShortcuts)
{
    Diagnostics diagnostics;
    const auto settings = parse(R"toml(
[tiling]
enabled = true
mode = "mac"
layout = "master-stack"
workspaces = 4
gap = 12
outer_gap = 0
master_size = 60
border = false
border_color = "#ff79c6"
run_as_admin = true

[tiling.keys]
float = "Ctrl+Alt+G"
promote = ""
workspace = "Alt+Win+#"

[[tiling.rule]]
app = "Calculator.exe"
do = "float"

[[tiling.rule]]
title = "Picture in picture"
class = "Chrome_WidgetWin_1"
do = "ignore"

[[tiling.rule]]
app = "Spotify.exe"
do = "workspace"
workspace = 3
)toml",
                                diagnostics);
    ASSERT_TRUE(settings.has_value()) << diagnostics.errorSummary("settings.toml");
    const TilingSettings& tiling = settings->tiling;
    EXPECT_TRUE(tiling.enabled);
    EXPECT_TRUE(settings->needsResidency());
    EXPECT_EQ(tiling.mode, TilingMode::Mac);
    EXPECT_EQ(tiling.layout, TilingLayout::MasterStack);
    EXPECT_EQ(tiling.workspaces, 4);
    EXPECT_EQ(tiling.gap, 12);
    EXPECT_EQ(tiling.outerGap, 0);
    EXPECT_EQ(tiling.masterPercent, 60);
    EXPECT_FALSE(tiling.border);
    EXPECT_EQ(tiling.borderColor->toHex(), "#FF79C6");
    EXPECT_TRUE(tiling.runAsAdmin);

    const auto key = [&](TilingCommand command) -> std::string {
        const auto found = std::ranges::find(tiling.keys, command, &TilingKey::command);
        return found == tiling.keys.end() ? "" : found->toString();
    };
    EXPECT_EQ(key(TilingCommand::Float), "Ctrl+Alt+G");
    EXPECT_EQ(key(TilingCommand::Promote), "");               // taken away
    EXPECT_EQ(key(TilingCommand::Workspace), "Alt+Win+#");
    EXPECT_EQ(key(TilingCommand::FocusLeft), "Ctrl+Alt+Left"); // left out: the default
    EXPECT_EQ(key(TilingCommand::SendToWorkspace), "Ctrl+Shift+Win+#");
    EXPECT_EQ(key(TilingCommand::Toggle), "");                 // none out of the box

    ASSERT_EQ(tiling.rules.size(), 3u);
    EXPECT_EQ(tiling.rules[0].app, "Calculator.exe");
    EXPECT_EQ(tiling.rules[1].action, TilingRuleAction::Ignore);
    EXPECT_EQ(tiling.rules[1].windowClass, "Chrome_WidgetWin_1");
    EXPECT_EQ(tiling.rules[2].workspace, 3);

    // What the settings app writes reads back the same.
    Diagnostics again;
    const auto written = parse(writeSettings(*settings), again);
    ASSERT_TRUE(written.has_value()) << again.errorSummary("settings.toml") << "\n" << writeSettings(*settings);
    EXPECT_EQ(written->tiling, tiling);
    // Untouched, nothing of it is written.
    EXPECT_EQ(writeSettings(Settings{}).find("[tiling]"), std::string::npos);
}

TEST(TilingSettings, ShortcutsWithANumberAndClashes)
{
    const auto go = TilingKey::parse(TilingCommand::Workspace, "Ctrl+Win+#");
    ASSERT_TRUE(go.has_value());
    EXPECT_TRUE(go->anyDigit);
    EXPECT_TRUE(go->overlaps(*KeyChord::parse("Win+Ctrl+7")));
    EXPECT_TRUE(go->overlaps(*KeyChord::parse("Ctrl+Win+Numpad3")));
    EXPECT_FALSE(go->overlaps(*KeyChord::parse("Ctrl+Win+0")));
    EXPECT_FALSE(go->overlaps(*KeyChord::parse("Ctrl+Win+Left")));
    EXPECT_FALSE(TilingKey::parse(TilingCommand::Workspace, "Ctrl+Win+1").has_value());  // needs #
    EXPECT_FALSE(TilingKey::parse(TilingCommand::FocusLeft, "Ctrl+Alt+#").has_value());  // takes none
    EXPECT_FALSE(TilingKey::parse(TilingCommand::FocusLeft, "Win+L").has_value());       // Windows'
    EXPECT_FALSE(TilingKey::parse(TilingCommand::Float, "Ctrl+Alt+Delete").has_value()); // Windows'
    EXPECT_FALSE(TilingKey::parse(TilingCommand::Workspace, "#").has_value());           // no modifier

    const char* bad[] = {
        "[tiling]\nlayout = \"spiral\"\n",
        "[tiling]\nworkspaces = 10\n",
        "[tiling]\ngap = -1\n",
        "[tiling.keys]\nfloat = \"Ctrl+Alt+Left\"\n",              // FocusLeft has it
        "[tiling.keys]\npromote = \"Ctrl+Win+3\"\n",               // inside Ctrl+Win+#
        "[tiling.keys]\nfloat = \"Ctrl+Alt+Nope\"\n",
        "[[tiling.rule]]\ndo = \"float\"\n",                       // which windows?
        "[[tiling.rule]]\napp = \"a.exe\"\ndo = \"workspace\"\n",  // which workspace?
        "[[tiling.rule]]\napp = \"a.exe\"\ndo = \"minimize\"\n",
        "[[hotkey]]\nkeys = \"Ctrl+Alt+F\"\naction = \"appearance.toggle\"\n[tiling]\nenabled = true\n",
    };
    for (const char* text : bad)
    {
        Diagnostics diagnostics;
        EXPECT_FALSE(parse(text, diagnostics).has_value()) << text;
    }
    // A command that does not exist is a warning, like any unknown key (a newer format, maybe).
    Diagnostics typo;
    EXPECT_TRUE(parse("[tiling.keys]\nfly = \"Ctrl+Alt+Y\"\n", typo).has_value());
    ASSERT_FALSE(typo.all().empty());
    EXPECT_EQ(typo.all()[0].key, "tiling.keys.fly");
}

TEST(DailyTime, CountsToTheNextAndFromTheLastOccurrence)
{
    const TimeOfDay seven{19, 0};
    EXPECT_EQ(secondsUntil(seven, (18 * 3600) + (59 * 60) + 59), 1);
    EXPECT_EQ(secondsUntil(seven, 19 * 3600), kSecondsPerDay); // exactly now: tomorrow
    EXPECT_EQ(secondsUntil(seven, (19 * 3600) + 1), kSecondsPerDay - 1);
    EXPECT_EQ(secondsUntil(TimeOfDay{0, 0}, kSecondsPerDay - 1), 1); // across midnight

    EXPECT_EQ(secondsSince(seven, 19 * 3600), 0);
    EXPECT_EQ(secondsSince(seven, (19 * 3600) + 90), 90);
    EXPECT_EQ(secondsSince(seven, (18 * 3600) + (59 * 60) + 59), kSecondsPerDay - 1);
    EXPECT_EQ(secondsSince(TimeOfDay{23, 59}, 30), 90); // across midnight
}

} // namespace
