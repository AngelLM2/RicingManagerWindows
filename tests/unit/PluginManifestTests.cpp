#include "rmw/domain/PluginApi.hpp"
#include "rmw/domain/PluginManifest.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/Tweaks.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace rmw::domain;

// The documented example (docs/12 §4 and docs/plugins).
constexpr std::string_view kNowPlaying = R"toml(
schema = 1

[plugin]
id          = "io.github.fulano.now-playing"
name        = "Now playing"
version     = "1.2.0"
api         = "^1.0"
authors     = ["fulano"]
license     = "MIT"
homepage    = "https://github.com/fulano/now-playing"
description = "What is playing, with play/pause."
kind        = ["bar.widget"]

[permissions]
providers = ["media"]
actions   = ["media.control"]
storage   = true

[entry]
script = "main.luau"

[widget]
ui        = "widget.slint"
component = "NowPlaying"

[widget.structs.Line]
text = "string"
bold = "bool"

[widget.properties]
title   = "string"
playing = "bool"
art     = "image"
lines   = "[Line]"

[widget.callbacks]
toggle = []
seek   = ["int"]

[config]
show_artist = { type = "bool", default = true, label = "Show the artist" }
max_width   = { type = "int", default = 240, min = 80, max = 600, label = "Maximum width" }
accent      = { type = "color", default = "palette.accent" }
style       = { type = "choice", choices = ["compact", "full"], default = "full" }
prefix      = { type = "string", default = "", max_length = 16 }
)toml";

struct Parsed
{
    rmw::Result<PluginManifest> manifest;
    Diagnostics diagnostics;
};

Parsed parse(std::string_view text)
{
    Diagnostics diagnostics;
    PluginParseOptions options;
    options.appVersion = SemVer{0, 4, 0, {}};
    auto manifest = parsePluginManifest(text, diagnostics, options);
    return {std::move(manifest), std::move(diagnostics)};
}

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
        text += d.toString("plugin.toml") + "\n";
    }
    return text;
}

// A minimal valid header of the given kinds, followed by `body`.
std::string plugin(std::string_view kinds, std::string_view body)
{
    return std::format("schema = 1\n[plugin]\nid = \"io.github.t.p\"\nname = \"t\"\nversion = \"1.0.0\"\n"
                       "api = \"^1.0\"\nkind = {}\n{}",
                       kinds, body);
}

std::string tweak(std::string_view key, std::string_view value, std::string_view type, std::string_view data)
{
    return plugin(R"(["tweak"])",
                  std::format("[[tweak]]\nkey = '{}'\nvalue = '{}'\ntype = \"{}\"\ndata = {}\n", key, value,
                              type, data));
}

TEST(PluginManifest, ReadsTheDocumentedExample)
{
    const auto [result, diagnostics] = parse(kNowPlaying);
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    EXPECT_TRUE(diagnostics.all().empty()) << dump(diagnostics);
    const PluginManifest& m = *result;
    EXPECT_EQ(m.id, "io.github.fulano.now-playing");
    EXPECT_EQ(m.version.toString(), "1.2.0");
    ASSERT_EQ(m.kinds.size(), 1u);
    EXPECT_TRUE(m.hasKind(PluginKind::BarWidget));
    EXPECT_EQ(m.permissions.providers, std::vector<std::string>{"media"});
    EXPECT_EQ(m.permissions.actions, std::vector<std::string>{"media.control"});
    EXPECT_TRUE(m.permissions.storage);
    EXPECT_TRUE(m.permissions.hasSensitive()); // media: what is playing is private
    EXPECT_EQ(m.script, "main.luau");

    ASSERT_TRUE(m.widget.has_value());
    EXPECT_EQ(m.widget->ui, "widget.slint");
    EXPECT_EQ(m.widget->component, "NowPlaying");
    ASSERT_EQ(m.widget->structs.size(), 1u);
    EXPECT_EQ(m.widget->structs[0].fields.size(), 2u);
    ASSERT_EQ(m.widget->properties.size(), 4u);
    const auto lines = std::ranges::find(m.widget->properties, "lines", &WidgetProperty::name);
    ASSERT_NE(lines, m.widget->properties.end());
    EXPECT_TRUE(lines->type.array);
    EXPECT_EQ(lines->type.structName, "Line");
    EXPECT_EQ(lines->type.toString(), "[Line]");
    ASSERT_EQ(m.widget->callbacks.size(), 2u);
    const auto seek = std::ranges::find(m.widget->callbacks, "seek", &WidgetCallback::name);
    ASSERT_NE(seek, m.widget->callbacks.end());
    EXPECT_EQ(seek->arguments, std::vector<WidgetType>{WidgetType::Int});

    ASSERT_EQ(m.config.fields.size(), 5u);
    const ConfigField* width = m.config.find("max_width");
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(width->type, ConfigType::Int);
    EXPECT_EQ(width->defaultValue, Value{std::int64_t{240}});
    EXPECT_EQ(width->min, 80.0);
    EXPECT_EQ(m.config.find("style")->defaultValue, Value{std::string("full")});
}

TEST(PluginManifest, RequiresTheHeader)
{
    const auto [result, diagnostics] = parse("schema = 1\n[plugin]\nname = \"x\"\n");
    ASSERT_FALSE(result.has_value());
    for (const std::string_view key : {"plugin.id", "plugin.version", "plugin.api", "plugin.kind"})
    {
        EXPECT_NE(find(diagnostics, key), nullptr) << key << "\n" << dump(diagnostics);
    }
    EXPECT_FALSE(parse("[plugin]\n").manifest.has_value()); // no schema
}

TEST(PluginManifest, ApiVersionMustMatch)
{
    const auto [result, diagnostics] = parse(R"(schema = 1
[plugin]
id = "io.github.t.p"
name = "t"
version = "1.0.0"
api = "^2.0"
kind = "rule"
[entry]
script = "main.luau"
)");
    ASSERT_FALSE(result.has_value());
    const auto* api = find(diagnostics, "plugin.api");
    ASSERT_NE(api, nullptr) << dump(diagnostics);
    EXPECT_NE(api->message.find("API 1.0.0"), std::string::npos) << api->message;
}

TEST(PluginManifest, UnknownPermissionsAreErrors)
{
    const auto [result, diagnostics] = parse(plugin(R"("rule")", R"([entry]
script = "main.luau"
[permissions]
providers = ["system.cpu", "files"]
actions = ["shell.run"]
)"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "permissions.providers"), nullptr) << dump(diagnostics);
    EXPECT_NE(find(diagnostics, "permissions.actions"), nullptr) << dump(diagnostics);
}

TEST(PluginManifest, NetworkDomainsArePublicNames)
{
    for (const std::string_view domain : {"127.0.0.1", "localhost", "router.lan", "printer.local", "com",
                                          "Api.Example.com", "-bad.example.com", "a..b.com", "example.com."})
    {
        EXPECT_FALSE(isValidNetworkDomain(domain)) << domain;
        const auto [result, diagnostics] = parse(
            plugin(R"("rule")",
                   std::format("[entry]\nscript = \"m.luau\"\n[permissions]\nnetwork = [\"{}\"]\n", domain)));
        EXPECT_FALSE(result.has_value()) << domain;
    }
    for (const std::string_view domain : {"api.open-meteo.com", "*.example.com", "x1.co"})
    {
        EXPECT_TRUE(isValidNetworkDomain(domain)) << domain;
    }

    const std::vector<std::string> allowed{"api.open-meteo.com", "*.example.com"};
    EXPECT_TRUE(networkDomainAllowed("API.open-meteo.com", allowed));
    EXPECT_TRUE(networkDomainAllowed("api.open-meteo.com.", allowed));
    EXPECT_TRUE(networkDomainAllowed("a.b.example.com", allowed));
    EXPECT_FALSE(networkDomainAllowed("example.com", allowed)); // "*." means subdomains
    EXPECT_FALSE(networkDomainAllowed("evil-example.com", allowed));
    EXPECT_FALSE(networkDomainAllowed("open-meteo.com", allowed));
    EXPECT_FALSE(networkDomainAllowed("api.open-meteo.com.evil.net", allowed));
    EXPECT_FALSE(networkDomainAllowed("", allowed));
}

TEST(PluginManifest, KindsAndTablesGoTogether)
{
    // A [widget] without the kind, an action without a script.
    auto [result, diagnostics] = parse(plugin(R"(["action"])", R"([[action]]
name = "focus"
[widget]
ui = "w.slint"
component = "W"
)"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "widget"), nullptr) << dump(diagnostics);
    EXPECT_NE(find(diagnostics, "entry.script"), nullptr) << dump(diagnostics);

    // A widget kind without [widget].
    EXPECT_FALSE(parse(plugin(R"("bar.widget")", "")).manifest.has_value());

    // A tweak plugin with a script: valid, but the script never runs.
    const auto tweakWithScript = parse(plugin(R"("tweak")", R"([entry]
script = "main.luau"
[[tweak]]
key = 'HKCU\Control Panel\Desktop'
value = "MenuShowDelay"
type = "string"
data = "100"
)"));
    ASSERT_TRUE(tweakWithScript.manifest.has_value()) << dump(tweakWithScript.diagnostics);
    EXPECT_NE(find(tweakWithScript.diagnostics, "entry"), nullptr);
}

TEST(PluginManifest, ActionsAreNamed)
{
    const auto [result, diagnostics] = parse(plugin(R"(["action"])", R"([entry]
script = "main.luau"
[[action]]
name = "focus"
label = "Focus mode"
[[action]]
name = "focus"
[[action]]
name = "Bad_Name"
)"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "action[1].name"), nullptr) << dump(diagnostics); // twice
    EXPECT_NE(find(diagnostics, "action[2].name"), nullptr) << dump(diagnostics);
}

TEST(PluginManifest, WidgetTypesAreChecked)
{
    const auto [result, diagnostics] = parse(plugin(R"("bar.widget")", R"([widget]
ui = "w.slint"
component = "W"
[widget.properties]
items = "[Missing]"
foreground = "color"
ok = "length"
[widget.callbacks]
ok = []
pick = ["image"]
)"));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "widget.properties.items"), nullptr) << dump(diagnostics);
    EXPECT_NE(find(diagnostics, "widget.properties.foreground"), nullptr) << dump(diagnostics);
    EXPECT_NE(find(diagnostics, "widget.callbacks.ok"), nullptr) << dump(diagnostics); // a property's name
    EXPECT_NE(find(diagnostics, "widget.callbacks.pick"), nullptr) << dump(diagnostics);
}

TEST(PluginManifest, FilesStayInThePackage)
{
    for (const std::string_view path :
         {"../main.luau", R"(C:\x\main.luau)", R"(\\server\share\main.luau)", "main.lua"})
    {
        const auto [result, diagnostics] =
            parse(plugin(R"("rule")", std::format("[entry]\nscript = '{}'\n", path)));
        EXPECT_FALSE(result.has_value()) << path;
        EXPECT_NE(find(diagnostics, "entry.script"), nullptr) << path << "\n" << dump(diagnostics);
    }
    const auto [result, diagnostics] = parse(plugin(R"("bar.widget")", R"([widget]
ui = "../other/widget.slint"
component = "W"
)"));
    EXPECT_NE(find(diagnostics, "widget.ui"), nullptr) << dump(diagnostics);
}

// ---- tweaks (acceptance M4: a tweak that writes to Run is rejected at validation) ----------------

TEST(PluginTweaks, AutostartKeysAreRejected)
{
    for (const std::string_view key :
         {R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Run)",
          R"(HKCU\Software\Microsoft\Windows\CurrentVersion\RunOnce)",
          R"(HKEY_CURRENT_USER\SOFTWARE\MICROSOFT\WINDOWS\CURRENTVERSION\RUN)",
          R"(HKCU/Software/Microsoft/Windows/CurrentVersion/Run)",
          R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\Run)",
          R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\..\..\Run)",
          R"(HKCU\Software\Classes\CLSID\{00000000-0000-0000-0000-000000000000}\InprocServer32)",
          R"(HKCU\Software\Policies\Microsoft\Windows\System)", R"(HKCU\Environment)",
          R"(HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon)",
          R"(HKLM\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)",
          R"(HKEY_LOCAL_MACHINE\SOFTWARE)", R"(Registry::HKCU\Control Panel\Desktop)"})
    {
        EXPECT_FALSE(validateTweakKey(key).has_value()) << key;
        const auto [result, diagnostics] = parse(tweak(key, "Anything", "dword", "1"));
        EXPECT_FALSE(result.has_value()) << key;
        EXPECT_NE(find(diagnostics, "tweak[0].key"), nullptr) << key << "\n" << dump(diagnostics);
    }
}

TEST(PluginTweaks, AllowedKeysAreNormalized)
{
    const auto key = validateTweakKey(R"(hkcu\software\microsoft\windows\currentversion\explorer\advanced)");
    ASSERT_TRUE(key.has_value()) << key.error().message();
    EXPECT_EQ(*key, R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)");
    const auto sub = validateTweakKey(R"(Control Panel/Desktop/WindowMetrics)");
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(*sub, R"(Control Panel\Desktop\WindowMetrics)");
    EXPECT_FALSE(validateTweakKey(R"(Control Panel\Desktop\ )").has_value());
    EXPECT_FALSE(validateTweakKey(R"(Control Panel\Deskto)").has_value());
    EXPECT_FALSE(validateTweakKey(R"(Control Panel\Desktop\Wind)"
                                  "\xC4\xB1"
                                  R"(ow)")
                     .has_value()); // non-ASCII

    const auto [result, diagnostics] = parse(tweak(
        R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "HideFileExt", "dword", "0"));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    ASSERT_EQ(result->tweaks.size(), 1u);
    EXPECT_EQ(result->tweaks[0].value, "HideFileExt");
    EXPECT_EQ(std::get<std::uint64_t>(result->tweaks[0].data), 0u);
}

TEST(PluginTweaks, ValuesThatStartProgramsOrBelongToModulesAreRejected)
{
    const std::string desktop = R"(Control Panel\Desktop)";
    for (const std::string_view name :
         {"SCRNSAVE.EXE", "shell", "Userinit", "Debugger", "Wallpaper", "WallpaperStyle"})
    {
        EXPECT_FALSE(validateTweakValueName(desktop, name).has_value()) << name;
    }
    EXPECT_FALSE(
        validateTweakValueName(R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)", "TaskbarAl")
            .has_value());
    EXPECT_FALSE(validateTweakValueName(R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                                        "AppsUseLightTheme")
                     .has_value());
    EXPECT_TRUE(validateTweakValueName(desktop, "MenuShowDelay").has_value());
    EXPECT_TRUE(validateTweakValueName(desktop, "").has_value()); // the default value
}

TEST(PluginTweaks, DataThatNamesProgramsIsRejected)
{
    for (const std::string_view data :
         {R"(C:\Users\x\evil.exe)", "evil.EXE", "rundll32 shell32,Control_RunDLL", R"(x.dll,Entry)",
          R"(\\server\share\a.wav)", "https://example.com/a", "%APPDATA%\\a.wav", "powershell -enc AAAA",
          "script.ps1", "a.vbs", "cmd /c echo", "x.hta"})
    {
        EXPECT_FALSE(validateTweakString(data).has_value()) << data;
    }
    for (const std::string_view data :
         {"100", R"(C:\Windows\Media\Windows Navigation Start.wav)", "0 0 0", "Segoe UI", "my.json"})
    {
        EXPECT_TRUE(validateTweakString(data).has_value()) << data;
    }
    const auto [result, diagnostics] =
        parse(tweak(R"(HKCU\Control Panel\Desktop)", "MenuShowDelay", "string", R"("C:\\x\\run.exe")"));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "tweak[0].data"), nullptr) << dump(diagnostics);
}

TEST(PluginTweaks, DwordRangeAndDuplicates)
{
    EXPECT_FALSE(
        parse(tweak(R"(HKCU\Control Panel\Mouse)", "MouseSpeed", "dword", "-1")).manifest.has_value());
    EXPECT_FALSE(parse(tweak(R"(HKCU\Control Panel\Mouse)", "MouseSpeed", "dword", "4294967296"))
                     .manifest.has_value());
    EXPECT_TRUE(parse(tweak(R"(HKCU\Control Panel\Mouse)", "MouseSpeed", "dword", "0xFFFFFFFF"))
                    .manifest.has_value());
    const auto [result, diagnostics] = parse(plugin(R"("tweak")", R"([[tweak]]
key = 'HKCU\Control Panel\Mouse'
value = "MouseSpeed"
type = "dword"
data = 1
[[tweak]]
key = 'hkcu\control panel\mouse'
value = "MouseSpeed"
type = "dword"
data = 2
)"));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(find(diagnostics, "tweak[1]"), nullptr) << dump(diagnostics);
}

// ---- settings ([config]) ------------------------------------------------------------------------

TEST(PluginConfig, LayersOverrideDefaultsAndBadValuesAreWarned)
{
    const auto parsed = parse(kNowPlaying);
    ASSERT_TRUE(parsed.manifest.has_value());
    const ConfigSchema& schema = parsed.manifest->config;

    const Value user{
        Value::Table{{"max_width", Value{std::int64_t{300}}}, {"style", Value{std::string("compact")}}}};
    const Value rice{Value::Table{{"max_width", Value{std::int64_t{1000}}},   // out of range
                                  {"show_artist", Value{std::string("yes")}}, // wrong type
                                  {"unknown", Value{true}},
                                  {"accent", Value{std::string("#ff0000")}}}};
    const Value item{Value::Table{{"prefix", Value{std::string("> ")}}}};
    Diagnostics diagnostics;
    const std::vector<const Value*> layers{&user, &rice, nullptr, &item};
    const Value resolved = resolvePluginConfig(schema, layers, diagnostics, "plugins.now-playing");

    EXPECT_EQ(*resolved.find("max_width"), Value{std::int64_t{300}});
    EXPECT_EQ(*resolved.find("style"), Value{std::string("compact")});
    EXPECT_EQ(*resolved.find("show_artist"), Value{true});
    EXPECT_EQ(*resolved.find("accent"), Value{std::string("#ff0000")});
    EXPECT_EQ(*resolved.find("prefix"), Value{std::string("> ")});
    EXPECT_EQ(resolved.find("unknown"), nullptr);
    EXPECT_FALSE(diagnostics.hasErrors());
    EXPECT_NE(find(diagnostics, "plugins.now-playing.max_width"), nullptr) << dump(diagnostics);
    EXPECT_NE(find(diagnostics, "plugins.now-playing.show_artist"), nullptr);
    EXPECT_NE(find(diagnostics, "plugins.now-playing.unknown"), nullptr);
}

TEST(PluginConfig, SchemaErrors)
{
    const auto [result, diagnostics] = parse(plugin(R"("rule")", R"([entry]
script = "main.luau"
[config]
a = { type = "int", min = 10, max = 1 }
b = { type = "choice", default = "x" }
c = { type = "color", default = "red" }
d = { type = "date" }
Bad = { type = "bool" }
e = { type = "int", default = 5, min = 10 }
)"));
    ASSERT_FALSE(result.has_value());
    for (const std::string_view key : {"config.a.max", "config.b.choices", "config.c.default",
                                       "config.d.type", "config.Bad", "config.e.default"})
    {
        EXPECT_NE(find(diagnostics, key), nullptr) << key << "\n" << dump(diagnostics);
    }
}

// ---- skins --------------------------------------------------------------------------------------

TEST(PluginSkin, FillsWhatTheRiceLeavesOut)
{
    const auto [result, diagnostics] = parse(plugin(R"("bar.skin")", R"([skin]
position = "top"
thickness = 36
radius = 12
margin = [6, 8]
background = { kind = "acrylic", tint = "palette.base", opacity = 0.6 }
left = ["start"]
center = ["clock"]
)"));
    ASSERT_TRUE(result.has_value()) << dump(diagnostics);
    ASSERT_TRUE(result->skin.has_value());
    const BarConfig& skin = *result->skin;
    EXPECT_EQ(skin.keys.size(), 7u);

    Diagnostics riceDiagnostics;
    const auto rice = parseRice(R"(schema = 1
[meta]
name = "r"
[palette]
base = "#101010"
[plugins]
requires = ["io.github.t.p"]
[[bar]]
skin = "io.github.t.p"
thickness = 48
center = ["windows"]
)",
                                riceDiagnostics);
    ASSERT_TRUE(rice.has_value()) << rice.error().message();
    ASSERT_EQ(rice->bars.size(), 1u);
    EXPECT_EQ(rice->bars[0].skin, "io.github.t.p");

    const BarConfig merged = applySkin(rice->bars[0], skin);
    EXPECT_EQ(merged.thickness, 48u); // the rice wins
    EXPECT_EQ(merged.position, BarPosition::Top);
    EXPECT_EQ(merged.radius, 12u);
    EXPECT_EQ(merged.margin, (std::array<std::uint32_t, 4>{6, 8, 6, 8}));
    EXPECT_EQ(merged.background.kind, BarBackground::Kind::Acrylic);
    ASSERT_TRUE(merged.background.tint.has_value());
    EXPECT_TRUE(merged.background.tint->isPalette());
    ASSERT_EQ(merged.left.size(), 1u);
    EXPECT_EQ(merged.left[0].id, "start");
    ASSERT_EQ(merged.center.size(), 1u);
    EXPECT_EQ(merged.center[0].id, "windows"); // the rice's zone
}

TEST(PluginSkin, MustSetSomething)
{
    EXPECT_FALSE(parse(plugin(R"("bar.skin")", "[skin]\n")).manifest.has_value());
}

// ---- permissions --------------------------------------------------------------------------------

TEST(PluginPermissions, ApprovalCoversOnlyWhatWasShown)
{
    PluginPermissions asked{{"system.cpu", "media"}, {"media.control"}, {"api.example.com"}, true};
    const PluginPermissions granted{{"system.cpu", "media"}, {"media.control"}, {"api.example.com"}, true};
    EXPECT_TRUE(asked.isWithin(granted));
    asked.providers.emplace_back("windows.list"); // an update asks for more
    EXPECT_FALSE(asked.isWithin(granted));
    const PluginPermissions both = asked.intersect(granted);
    EXPECT_EQ(both.providers, (std::vector<std::string>{"system.cpu", "media"}));
    EXPECT_TRUE(both.storage);
    const PluginPermissions quiet{{"system.cpu"}, {}, {}, false};
    const PluginPermissions closes{{}, {"window.close"}, {}, false};
    const PluginPermissions online{{}, {}, {"api.example.com"}, false};
    EXPECT_FALSE(quiet.hasSensitive());
    EXPECT_TRUE(closes.hasSensitive());
    EXPECT_TRUE(online.hasSensitive());
}

TEST(PluginApi, NamesAreKnown)
{
    EXPECT_NE(findPluginProvider("windows.list"), nullptr);
    EXPECT_TRUE(findPluginProvider("windows.list")->sensitive);
    EXPECT_EQ(findPluginProvider("files"), nullptr);
    EXPECT_NE(findPluginAction("bar.toggle"), nullptr);
    EXPECT_EQ(findPluginAction("restore.original"), nullptr); // not for plugins
    EXPECT_TRUE(isValidPluginMemberName("focus-mode"));
    EXPECT_FALSE(isValidPluginMemberName("focus--mode"));
    EXPECT_FALSE(isValidPluginMemberName("Focus"));
    EXPECT_FALSE(isValidPluginMemberName("focus_mode"));
}

} // namespace
