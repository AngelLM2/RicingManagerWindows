#include "rmw/base/Limits.hpp"
#include "rmw/domain/RiceParser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

using namespace rmw::domain;

// The example from docs/03 §6, plus a palette.
constexpr std::string_view kFullRice = R"toml(
schema = 1

[meta]
name    = "Tokyo Night"
author  = "fulano"
version = "1.0.0"
license = "CC-BY-4.0"
min_app = "0.1.0"

[palette]
base = "#1a1b26"
text = "#c0caf5"

[appearance]
mode              = "dark"
accent            = "#7aa2f7"
transparency      = true
accent_on_taskbar = true

[wallpaper]
mode       = "per-monitor"
background = "#1a1b26"

[wallpaper.policy]
pause_on_fullscreen = true
fps_limit           = 30
mute                = true

[[wallpaper.monitor]]
match          = "primary"
kind           = "video"
source         = "assets/city.mp4"
fallback_image = "assets/city.jpg"

[[wallpaper.monitor]]
match  = "all"
kind   = "image"
source = "assets/mountains.jpg"
fit    = "center"

[cursors]
source = "assets/cursors"
size   = 48
[cursors.map]
normal = "pointer.cur"
busy   = "busy.ani"
text   = "beam.cur"

[taskbar]
alignment = "left"
autohide  = false
task_view = false
search    = "icon"

[[bar]]
monitor    = "all"
position   = "top"
mode       = "float"
thickness  = 36
length     = "80%"
margin     = [8, 8, 0, 8]
radius     = 12
background = { kind = "acrylic", tint = "palette.base", opacity = 0.75 }
foreground = "palette.text"
hide_native_taskbar = true
left   = ["start", "workspaces", "tasklist"]
center = [{ id = "clock", format = "%H:%M", seconds = false }]
right  = ["io.github.fulano.now-playing", "tray", "volume", "battery"]

[plugins]
requires = ["io.github.fulano.now-playing@^1.2"]

[plugins."io.github.fulano.now-playing"]
show_artist = true
max_width   = 260
)toml";

struct Parsed
{
    rmw::Result<Rice> rice;
    Diagnostics diagnostics;
};

Parsed parse(std::string_view text, const RiceParseOptions& options = {})
{
    Diagnostics diagnostics;
    auto rice = parseRice(text, diagnostics, options);
    return {std::move(rice), std::move(diagnostics)};
}

// The first diagnostic whose key is `key`, or nullptr.
const Diagnostic* find(const Diagnostics& diagnostics, std::string_view key)
{
    const auto& all = diagnostics.all();
    const auto it = std::ranges::find_if(all, [&](const Diagnostic& d) { return d.key == key; });
    return it == all.end() ? nullptr : &*it;
}

std::string dump(const Diagnostics& diagnostics)
{
    std::string text;
    for (const auto& d : diagnostics.all())
    {
        text += d.toString("rice.toml") + "\n";
    }
    return text;
}

// A minimal valid rice followed by `body`.
std::string rice(std::string_view body)
{
    return std::string("schema = 1\n[meta]\nname = \"t\"\n") + std::string(body);
}

TEST(RiceParser, ReadsTheDocumentedExample)
{
    const auto [result, diagnostics] = parse(kFullRice);
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    // Only the notes about what this version cannot do yet: video, and a fit per monitor.
    ASSERT_EQ(diagnostics.all().size(), 2u) << dump(diagnostics);
    for (const auto& d : diagnostics.all())
    {
        EXPECT_EQ(d.severity, Severity::Warning);
        EXPECT_EQ(d.key, "wallpaper.monitor");
    }
    const Rice& r = *result;

    EXPECT_EQ(r.meta.name, "Tokyo Night");
    EXPECT_EQ(r.meta.version->toString(), "1.0.0");
    ASSERT_EQ(r.palette.size(), 2u);
    EXPECT_EQ(r.findPalette("text")->color, Color::parse("#c0caf5").value());

    ASSERT_TRUE(r.appearance.has_value());
    EXPECT_EQ(r.appearance->mode, ThemeMode::Dark);
    EXPECT_EQ(r.appearance->accent, Color::parse("#7aa2f7").value());
    EXPECT_FALSE(r.appearance->schedule.has_value());

    ASSERT_TRUE(r.wallpaper.has_value());
    EXPECT_EQ(r.wallpaper->policy.fpsLimit, 30u);
    EXPECT_FALSE(r.wallpaper->policy.pauseOnBattery.has_value());
    ASSERT_EQ(r.wallpaper->monitors.size(), 2u);
    EXPECT_EQ(r.wallpaper->monitors[0].kind, WallpaperKind::Video);
    EXPECT_EQ(r.wallpaper->monitors[0].fallbackImage, "assets/city.jpg");
    EXPECT_EQ(r.wallpaper->monitors[1].match.kind, MonitorSelector::Kind::All);
    EXPECT_EQ(r.wallpaper->monitors[1].fit, WallpaperFit::Center);

    ASSERT_TRUE(r.cursors.has_value());
    EXPECT_EQ(r.cursors->size, 48u);
    ASSERT_EQ(r.cursors->map.size(), 3u);
    const auto busy = std::ranges::find(r.cursors->map, CursorRole::Busy, &CursorAssignment::role);
    ASSERT_NE(busy, r.cursors->map.end());
    EXPECT_EQ(busy->file, "busy.ani");

    EXPECT_EQ(r.taskbar->search, TaskbarSearch::Icon);
    EXPECT_EQ(r.taskbar->alignment, TaskbarAlignment::Left);

    ASSERT_EQ(r.bars.size(), 1u);
    const BarConfig& bar = r.bars[0];
    EXPECT_EQ(bar.position, BarPosition::Top);
    EXPECT_EQ(bar.mode, BarMode::Float);
    EXPECT_TRUE(bar.length.percent);
    EXPECT_DOUBLE_EQ(bar.length.value, 80.0);
    EXPECT_EQ(bar.margin[1], 8u);
    EXPECT_EQ(bar.margin[2], 0u);
    EXPECT_EQ(bar.background.kind, BarBackground::Kind::Acrylic);
    EXPECT_EQ(bar.background.tint->toString(), "palette.base");
    EXPECT_DOUBLE_EQ(*bar.background.opacity, 0.75);
    EXPECT_TRUE(bar.hideNativeTaskbar);
    ASSERT_EQ(bar.center.size(), 1u);
    EXPECT_EQ(bar.center[0].id, "clock");
    ASSERT_NE(bar.center[0].config.find("format"), nullptr);
    EXPECT_EQ(std::get<std::string>(bar.center[0].config.find("format")->data), "%H:%M");
    EXPECT_TRUE(bar.right[0].isPlugin());
    EXPECT_FALSE(bar.right[1].isPlugin());

    ASSERT_EQ(r.pluginRequirements.size(), 1u);
    EXPECT_TRUE(r.pluginRequirements[0].version.matches(SemVer::parse("1.4.0").value()));
    ASSERT_EQ(r.pluginSettings.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(r.pluginSettings[0].config.find("max_width")->data), 260);
}

TEST(RiceParser, MinimalRiceManagesNothing)
{
    const auto [result, diagnostics] = parse(rice(""));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    EXPECT_FALSE(result->appearance || result->wallpaper || result->cursors || result->taskbar);
    EXPECT_TRUE(result->bars.empty());
}

TEST(RiceParser, SyntaxErrorsCarryLineAndColumn)
{
    const auto [result, diagnostics] = parse("schema = 1\n[meta]\nname = \"x\n");
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(diagnostics.all().size(), 1u);
    EXPECT_EQ(diagnostics.all()[0].line, 3u);
    EXPECT_GT(diagnostics.all()[0].column, 0u);
}

TEST(RiceParser, SemanticErrorsPointAtTheValue)
{
    const auto [result, diagnostics] = parse(rice("[appearance]\nmode = \"darkest\"\n"));
    ASSERT_FALSE(result.has_value());
    const Diagnostic* d = find(diagnostics, "appearance.mode");
    ASSERT_NE(d, nullptr) << dump(diagnostics);
    EXPECT_EQ(d->line, 5u);
    EXPECT_EQ(d->column, 8u);
    EXPECT_NE(d->message.find("light | dark | system | schedule"), std::string::npos) << d->message;
    EXPECT_NE(result.error().message().find("rice.toml:5:8"), std::string::npos) << result.error().message();
}

TEST(RiceParser, UnknownKeysAreWarningsNotErrors)
{
    const auto [result, diagnostics] =
        parse(rice("[apperance]\nmode = \"dark\"\n[taskbar]\nalign = \"left\"\n"));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    const Diagnostic* section = find(diagnostics, "apperance");
    ASSERT_NE(section, nullptr) << dump(diagnostics);
    EXPECT_EQ(section->severity, Severity::Warning);
    EXPECT_EQ(section->line, 4u);
    ASSERT_NE(find(diagnostics, "taskbar.align"), nullptr) << dump(diagnostics);
}

TEST(RiceParser, RequiresSchemaAndName)
{
    EXPECT_NE(find(parse("[meta]\nname = \"x\"\n").diagnostics, "schema"), nullptr);
    EXPECT_NE(find(parse("schema = 1\n[meta]\nauthor = \"x\"\n").diagnostics, "meta.name"), nullptr);
    EXPECT_NE(find(parse("schema = 1\n").diagnostics, "meta"), nullptr);
}

TEST(RiceParser, NewerSchemaAsksForAnUpdate)
{
    const auto [result, diagnostics] = parse("schema = 2\n[meta]\nname = \"x\"\n");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "schema")->message.find("update"), std::string::npos);
}

TEST(RiceParser, MinAppIsComparedWithThisBuild)
{
    RiceParseOptions options;
    options.appVersion = SemVer::parse("0.4.0").value();
    const std::string base = "schema = 1\n[meta]\nname = \"t\"\nmin_app = ";
    EXPECT_TRUE(parse(base + "\"0.4.0\"\n", options).rice.has_value());
    EXPECT_FALSE(parse(base + "\"0.5.0\"\n", options).rice.has_value());
}

TEST(RiceParser, RejectsUnsafeAndWrongTypePaths)
{
    const char* bad[] = {
        "source = \"../../Windows/x.jpg\"", "source = \"C:/x.jpg\"",         "source = \"assets/x.exe\"",
        "source = \"assets/CON.jpg\"",      "source = \"assets/x.jpg:ads\"", "source = 42",
    };
    for (const char* line : bad)
    {
        const auto [result, diagnostics] = parse(rice("[[wallpaper.monitor]]\n" + std::string(line) + "\n"));
        EXPECT_FALSE(result.has_value()) << line;
        EXPECT_NE(find(diagnostics, "wallpaper.monitor[0].source"), nullptr) << line << "\n"
                                                                             << dump(diagnostics);
    }
}

TEST(RiceParser, WallpaperKindsCheckTheirOwnFields)
{
    EXPECT_FALSE(parse(rice("[[wallpaper.monitor]]\nkind = \"color\"\n")).rice.has_value()); // color missing
    EXPECT_TRUE(parse(rice("[[wallpaper.monitor]]\nkind = \"color\"\ncolor = \"#000\"\n")).rice.has_value());
    EXPECT_FALSE(parse(rice("[[wallpaper.monitor]]\nkind = \"video\"\nsource = \"a.mp4\"\nfit = \"tile\"\n"))
                     .rice.has_value());

    const auto [result, diagnostics] =
        parse(rice("[[wallpaper.monitor]]\nsource = \"a.png\"\ninterval = 60\n"));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    const Diagnostic* d = find(diagnostics, "wallpaper.monitor[0].interval");
    ASSERT_NE(d, nullptr) << dump(diagnostics);
    EXPECT_EQ(d->severity, Severity::Warning);
    EXPECT_NE(d->message.find("does not apply"), std::string::npos);

    const auto slideshow = parse(
        rice("[[wallpaper.monitor]]\nkind = \"slideshow\"\nsource = \"assets/pics\"\ninterval = 600\n"));
    ASSERT_TRUE(slideshow.rice.has_value()) << dump(slideshow.diagnostics);
    EXPECT_EQ(slideshow.rice->wallpaper->monitors[0].intervalSeconds, 600u);
}

TEST(RiceParser, SpanAndMonitorRulesDoNotMix)
{
    EXPECT_TRUE(parse(rice("[wallpaper]\nmode = \"span\"\nsource = \"wide.jpg\"\n")).rice.has_value());
    EXPECT_FALSE(parse(rice("[wallpaper]\nmode = \"span\"\n")).rice.has_value());
    EXPECT_FALSE(parse(rice("[wallpaper]\nmode = \"span\"\nsource = \"wide.jpg\"\n[[wallpaper.monitor]]\n"
                            "source = \"a.jpg\"\n"))
                     .rice.has_value());
    EXPECT_FALSE(parse(rice("[wallpaper]\nsource = \"wide.jpg\"\n")).rice.has_value());
}

TEST(RiceParser, DuplicateMonitorRuleIsWarned)
{
    const auto [result, diagnostics] =
        parse(rice("[[wallpaper.monitor]]\nsource = \"a.jpg\"\n[[wallpaper.monitor]]\nsource = \"b.jpg\"\n"));
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(find(diagnostics, "wallpaper.monitor[1].match"), nullptr) << dump(diagnostics);
}

TEST(RiceParser, ScheduleDefaultsAndChecks)
{
    const auto defaults = parse(rice("[appearance]\nmode = \"schedule\"\n"));
    ASSERT_TRUE(defaults.rice.has_value());
    EXPECT_EQ(defaults.rice->appearance->schedule->darkAt, (TimeOfDay{19, 0}));

    EXPECT_FALSE(
        parse(rice("[appearance]\nmode = \"schedule\"\n[appearance.schedule]\nlight_at = \"19:00\"\n"))
            .rice.has_value()); // same as the default dark_at
    EXPECT_FALSE(parse(rice("[appearance.schedule]\nlight_at = \"7h\"\n")).rice.has_value());
}

TEST(RiceParser, CursorSizeAndRoles)
{
    EXPECT_FALSE(parse(rice("[cursors]\nsource = \"c\"\nsize = 40\n")).rice.has_value());
    EXPECT_FALSE(parse(rice("[cursors]\nsource = \"c\"\nsize = 512\n")).rice.has_value());
    EXPECT_FALSE(
        parse(rice("[cursors]\nsource = \"c\"\n[cursors.map]\narrow = \"a.cur\"\n")).rice.has_value());
    EXPECT_FALSE(
        parse(rice("[cursors]\nsource = \"c\"\n[cursors.map]\nnormal = \"a.png\"\n")).rice.has_value());
    EXPECT_FALSE(parse(rice("[cursors]\nsize = 32\n")).rice.has_value()); // source missing
}

TEST(RiceParser, BarShapes)
{
    const auto [result, diagnostics] =
        parse(rice("[[bar]]\nlength = 600\nmargin = 4\nbackground = \"mica\"\n[[bar]]\nmargin = [2, 6]\n"));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    ASSERT_EQ(result->bars.size(), 2u);
    EXPECT_FALSE(result->bars[0].length.percent);
    EXPECT_DOUBLE_EQ(result->bars[0].length.value, 600.0);
    EXPECT_EQ(result->bars[0].margin[3], 4u);
    EXPECT_EQ(result->bars[0].background.kind, BarBackground::Kind::Mica);
    EXPECT_EQ(result->bars[1].margin[0], 2u);
    EXPECT_EQ(result->bars[1].margin[1], 6u);
    EXPECT_EQ(result->bars[1].position, BarPosition::Bottom);

    for (const char* body :
         {"[[bar]]\nlength = \"0%\"\n", "[[bar]]\nlength = \"150%\"\n", "[[bar]]\nlength = \"%\"\n",
          "[[bar]]\nlength = \"50 %\"\n", "[[bar]]\nmargin = [1, 2, 3]\n", "[[bar]]\nmargin = -1\n",
          "[[bar]]\nthickness = 4\n", "[bar]\nposition = \"top\"\n", "[[bar]]\nleft = [\"Clock!\"]\n",
          "[[bar]]\nleft = [{ format = \"x\" }]\n", "[[bar]]\nforeground = \"palette.nope\"\n",
          "[[bar]]\nbackground = { kind = \"acrylic\", opacity = 1.5 }\n"})
    {
        EXPECT_FALSE(parse(rice(body)).rice.has_value()) << body;
    }
}

TEST(RiceParser, PluginWidgetsShouldBeRequired)
{
    const auto [result, diagnostics] = parse(rice("[[bar]]\nright = [\"io.github.x.weather\"]\n"));
    ASSERT_TRUE(result.has_value());
    const Diagnostic* d = find(diagnostics, "bar[0].right[0]");
    ASSERT_NE(d, nullptr) << dump(diagnostics);
    EXPECT_EQ(d->severity, Severity::Warning);
}

TEST(RiceParser, PluginRequirements)
{
    EXPECT_TRUE(
        parse(rice("[plugins]\nrequires = [\"io.github.a.b\", \"io.github.c.d@~2.1\"]\n")).rice.has_value());
    for (const char* body :
         {"[plugins]\nrequires = [\"bad\"]\n", "[plugins]\nrequires = [\"io.a.b@2\", \"io.a.b\"]\n",
          "[plugins]\nrequires = [\"io.a.b@banana\"]\n", "[plugins]\nrequires = [1]\n",
          "[plugins]\nnot_a_plugin = 1\n"})
    {
        EXPECT_FALSE(parse(rice(body)).rice.has_value()) << body;
    }
}

TEST(RiceParser, RejectsSpoofingCharactersInNames)
{
    // U+202E RIGHT-TO-LEFT OVERRIDE would let "gpj.exe" render as "exe.jpg".
    const std::string rightToLeftOverride = "\xE2\x80\xAE"; // NOLINT(misc-misleading-bidirectional)
    EXPECT_FALSE(parse("schema = 1\n[meta]\nname = \"a" + rightToLeftOverride + "b\"\n").rice.has_value());
    EXPECT_FALSE(parse("schema = 1\n[meta]\nname = \"a\\u0007b\"\n").rice.has_value());
    EXPECT_TRUE(parse("schema = 1\n[meta]\nname = \"Ação 夜\"\n").rice.has_value());
}

TEST(RiceParser, EnforcesTheSizeLimit)
{
    std::string text = rice("");
    text += "# " + std::string(rmw::limits::kManifestMaxBytes, 'x') + "\n";
    const auto [result, diagnostics] = parse(text);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(diagnostics.all()[0].message.find("too large"), std::string::npos);
}

// Regression: toml++ finishes a parse (and destroys a tree) recursively, so these documents
// would exhaust the stack if they reached it. They must be rejected up front, quickly.
TEST(RiceParser, DeepNestingIsRejectedBeforeParsing)
{
    std::string dottedKey = "x";
    std::string header = "[x";
    std::string arrays;
    std::string inlineTables;
    for (int i = 0; i < 20'000; ++i)
    {
        dottedKey += ".a";
        header += ".a";
    }
    for (int i = 0; i < 300; ++i)
    {
        arrays += "[";
        inlineTables += "{a=";
    }
    const std::string documents[] = {
        rice(dottedKey + " = 1\n"),
        rice(header + "]\n"),
        rice("[[" + header.substr(1) + "]]\n"),
        rice("v = " + arrays + "\n"),
        rice("v = " + inlineTables + "1\n"),
        rice("[plugins.\"io.a.b\"]\n" + dottedKey + " = 1\n"),
    };
    for (const auto& document : documents)
    {
        const auto [result, diagnostics] = parse(document);
        ASSERT_FALSE(result.has_value());
        ASSERT_FALSE(diagnostics.all().empty());
        EXPECT_NE(diagnostics.all()[0].message.find("too deep"), std::string::npos) << dump(diagnostics);
    }
}

TEST(RiceParser, NestingUpToTheLimitIsAccepted)
{
    // Depth 32 exactly: [plugins] (1) . "io.a.b" (2), then 29 tables (3..31) and a value (32).
    std::string key = "k";
    for (int i = 0; i < 29; ++i)
    {
        key += ".k";
    }
    const std::string ok =
        rice("[plugins]\nrequires = [\"io.a.b\"]\n[plugins.\"io.a.b\"]\n" + key + " = 1\n");
    const auto accepted = parse(ok);
    EXPECT_TRUE(accepted.rice.has_value()) << dump(accepted.diagnostics);

    const std::string tooDeep =
        rice("[plugins]\nrequires = [\"io.a.b\"]\n[plugins.\"io.a.b\"]\n" + key + ".k = 1\n");
    EXPECT_FALSE(parse(tooDeep).rice.has_value());
}

TEST(RiceParser, DepthScanIsNotFooledByStringsAndComments)
{
    // Dots inside strings, comments and values never add depth.
    const std::string dots(200, '.');
    const std::string text = rice("# " + dots + "\n[appearance]\naccent = \"#000000\" # a.b.c.d\n[\"" + dots +
                                  "\"]\nv = '" + dots + "'\nw = \"\"\"\n" + dots + "\n\"\"\"\nf = 1.5\n");
    const auto [result, diagnostics] = parse(text);
    EXPECT_TRUE(result.has_value()) << dump(diagnostics);
}

TEST(RiceParser, InvalidUtf8IsRejected)
{
    EXPECT_FALSE(parse("schema = 1\n[meta]\nname = \"\xC3\x28\"\n").rice.has_value());
}

} // namespace
