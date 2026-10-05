#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/domain/Settings.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace
{

using namespace rmw::domain;

// Every section and every kind of value the writer has to get right.
constexpr std::string_view kEverything = R"toml(
schema = 1

[meta]
id          = "io.github.someone.tokyo-night"
name        = "Tokyo \"Night\""
author      = "fulano"
description = "Line one\nLine two\tand a tab, a \\ and ü"
version     = "1.2.3"
license     = "CC-BY-4.0"
min_app     = "0.1.0"

[palette]
base = "#1a1b26"
glass = "#1a1b2680"
text = "#c0caf5"

[appearance]
mode              = "schedule"
accent            = "#7aa2f7"
transparency      = true
accent_on_taskbar = false

[appearance.schedule]
light_at = "06:30"
dark_at  = "18:45"

[wallpaper]
mode       = "per-monitor"
background = "#1a1b26"

[wallpaper.policy]
pause_on_fullscreen = true
pause_on_battery    = false
pause_when_covered  = true
fps_limit           = 24
mute                = true

[[wallpaper.monitor]]
match          = "primary"
kind           = "video"
source         = "assets/city.mp4"
fallback_image = "assets/city.jpg"
fit            = "fit"

[[wallpaper.monitor]]
match    = "left"
kind     = "slideshow"
source   = "assets/slides"
interval = 600
shuffle  = true

[[wallpaper.monitor]]
match = "right"
kind  = "color"
color = "#000000"

[[wallpaper.monitor]]
match  = "all"
source = "assets/mountains.jpg"
fit    = "center"

[cursors]
source = "assets/cursors"
size   = 48
[cursors.map]
normal = "pointer.cur"
busy   = "busy.ani"

[taskbar]
alignment = "left"
autohide  = false
task_view = true
search    = "hidden"

[plugins]
requires = ["io.github.someone.now-playing@^1.2", "io.github.someone.weather"]

[plugins."io.github.someone.weather"]
city  = "São Paulo"
units = "metric"
every = 15
scale = 1.5
whole = 2.0
tags  = ["a", "b"]
style = { bold = true, size = 12 }

[[bar]]
monitor    = "all"
position   = "top"
mode       = "float"
thickness  = 36
length     = "80%"
margin     = [8, 8, 0, 8]
radius     = 12
background = { kind = "acrylic", tint = "palette.glass", opacity = 0.75 }
foreground = "palette.text"
autohide   = true
hide_native_taskbar = true
left   = ["start", "desktops", "windows"]
center = [{ id = "clock", format = "%H:%M" }]
right  = ["tray", { id = "io.github.someone.now-playing" }, { id = "io.github.someone.weather", compact = true }]

[[bar]]
monitor   = "primary"
position  = "left"
thickness = 48
length    = 600
)toml";

Rice parse(std::string_view text)
{
    Diagnostics diagnostics;
    auto rice = parseRice(text, diagnostics);
    EXPECT_TRUE(rice.has_value()) << (rice ? "" : rice.error().message()) << "\n" << text;
    return rice ? std::move(*rice) : Rice{};
}

TEST(RiceWriter, WritesWhatReadsBackTheSame)
{
    const Rice original = parse(kEverything);
    const std::string written = writeRice(original);
    const Rice again = parse(written);
    // Writing what was read back gives the same text: nothing was lost or changed on the way.
    EXPECT_EQ(writeRice(again), written);

    EXPECT_EQ(again.meta.name, "Tokyo \"Night\"");
    EXPECT_EQ(again.meta.description, "Line one\nLine two\tand a tab, a \\ and \xC3\xBC");
    ASSERT_TRUE(again.appearance && again.appearance->schedule);
    EXPECT_EQ(again.appearance->schedule->lightAt.hour, 6);
    EXPECT_EQ(again.appearance->schedule->darkAt.minute, 45);
    ASSERT_TRUE(again.wallpaper);
    ASSERT_EQ(again.wallpaper->monitors.size(), 4u);
    EXPECT_EQ(again.wallpaper->monitors[0].fallbackImage, "assets/city.jpg");
    EXPECT_EQ(again.wallpaper->monitors[1].intervalSeconds, 600u);
    EXPECT_EQ(again.wallpaper->policy.fpsLimit, 24u);
    ASSERT_EQ(again.pluginRequirements.size(), 2u);
    EXPECT_EQ(again.pluginRequirements[0].version.toString(), "^1.2");
    ASSERT_EQ(again.pluginSettings.size(), 1u);
    EXPECT_EQ(again.pluginSettings[0].config, original.pluginSettings[0].config); // floats stay floats
    ASSERT_EQ(again.bars.size(), 2u);
    EXPECT_EQ(again.bars[0].center[0].config, original.bars[0].center[0].config);
    EXPECT_EQ(again.bars[0].right[1].config, original.bars[0].right[1].config); // { id } only
    EXPECT_TRUE(again.bars[0].right[0].config.isNull());                        // a plain name
    EXPECT_FALSE(again.bars[1].length.percent);
    EXPECT_EQ(again.bars[1].length.value, 600.0);
    // The second bar names only some keys; the rest stay unwritten (a skin could fill them).
    EXPECT_EQ(again.bars[1].keys, original.bars[1].keys);
}

TEST(RiceWriter, ANewBarWithoutKeysWritesThemAll)
{
    Rice rice;
    rice.meta.name = "New";
    BarConfig bar;
    bar.position = BarPosition::Right;
    bar.left.push_back({"clock", {}});
    rice.bars.push_back(bar);
    const Rice again = parse(writeRice(rice));
    ASSERT_EQ(again.bars.size(), 1u);
    EXPECT_EQ(again.bars[0].position, BarPosition::Right);
    EXPECT_EQ(again.bars[0].keys.size(), 12u); // all but "foreground", which it does not set
    ASSERT_EQ(again.bars[0].left.size(), 1u);
    EXPECT_EQ(again.bars[0].left[0].id, "clock");
}

TEST(SettingsWriter, WritesWhatReadsBackTheSame)
{
    constexpr std::string_view text = R"toml(
schema = 1

[core]
keep_in_tray = true

[ui]
language = "pt-BR"

[[hotkey]]
keys = "Ctrl+Win+Alt+N"
action = "rice.apply"
rice = 'C:\Rices\night "dark"'

[[hotkey]]
keys = "Win+Alt+D"
action = "appearance.toggle"

[[rule]]
when = "time"
at = "07:05"
action = "appearance.light"

[[rule]]
when = "monitor-connected"
monitor = "edid:ABC1234"
action = "io.github.someone.focus:on"

[[rule]]
when = "battery"
action = "appearance.dark"

[[rule]]
when = "ac"
action = "launch.app"
app = "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"
)toml";
    constexpr std::array<std::string_view, 3> actions = {"rice.apply", "appearance.toggle",
                                                         "appearance.light"};
    const std::array<std::string_view, 5> known = {actions[0], actions[1], actions[2], "appearance.dark",
                                                   "launch.app"};
    Diagnostics diagnostics;
    auto settings = parseSettings(text, diagnostics, known);
    ASSERT_TRUE(settings.has_value()) << settings.error().message();
    const std::string written = writeSettings(*settings);
    Diagnostics again;
    auto reread = parseSettings(written, again, known);
    ASSERT_TRUE(reread.has_value()) << reread.error().message() << "\n" << written;
    EXPECT_EQ(writeSettings(*reread), written);
    EXPECT_TRUE(reread->keepInTray);
    EXPECT_EQ(reread->language, "pt-BR");
    ASSERT_EQ(reread->hotkeys.size(), 2u);
    EXPECT_EQ(reread->hotkeys[0].action.ricePath, "C:\\Rices\\night \"dark\"");
    EXPECT_EQ(reread->hotkeys[1].keys.toString(), "Alt+Win+D");
    ASSERT_EQ(reread->rules.size(), 4u);
    EXPECT_EQ(reread->rules[3].action.app, "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App");
    EXPECT_EQ(reread->rules[0].at->minute, 5);
    EXPECT_EQ(reread->rules[1].monitor->toString(), "edid:ABC1234");
    EXPECT_EQ(reread->rules[1].action.name, "io.github.someone.focus:on");
}

TEST(SettingsWriter, AnUnknownLanguageIsAnError)
{
    Diagnostics diagnostics;
    EXPECT_FALSE(parseSettings("[ui]\nlanguage = \"klingon\"\n", diagnostics, {}).has_value());
}

TEST(SettingsWriter, LaunchAppNeedsTheApp)
{
    constexpr std::array<std::string_view, 1> known = {"launch.app"};
    Diagnostics diagnostics;
    EXPECT_FALSE(
        parseSettings("[[hotkey]]\nkeys = \"Win+Alt+T\"\naction = \"launch.app\"\n", diagnostics, known));
}

TEST(RiceWriter, QuotesWhatTomlNeedsQuoted)
{
    EXPECT_EQ(tomlQuote("a\"b\\c\nd\x01"), "\"a\\\"b\\\\c\\nd\\u0001\"");
    EXPECT_EQ(tomlKey("plain-key_1"), "plain-key_1");
    EXPECT_EQ(tomlKey("io.github.x"), "\"io.github.x\"");
    EXPECT_EQ(tomlKey(""), "\"\"");
    EXPECT_EQ(tomlValue(Value{1.0}), "1.0");
    EXPECT_EQ(tomlValue(Value{std::int64_t{1}}), "1");
    EXPECT_EQ(tomlValue(Value{Value::Table{}}), "{}");
}

} // namespace
