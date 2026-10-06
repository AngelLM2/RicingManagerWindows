#include "PluginWidgets.hpp"
#include "StartMenu.hpp"
#include "Surface.hpp"
#include "Widgets.hpp"

#include "rmw/bar/Source.hpp"
#include "rmw/bar/WidgetSource.hpp"

#include <gtest/gtest.h>
#include <slint-interpreter.h>
#include <slint-platform.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>

// The bar's drawing without windows: a platform whose windows are only a renderer and a size.
namespace
{

using rmw::barapp::Surface;
using slint::interpreter::Value;

class HeadlessWindow final : public slint::platform::WindowAdapter
{
public:
    slint::platform::AbstractRenderer& renderer() override { return m_renderer; }
    slint::PhysicalSize size() override { return m_size; }

    void resize(std::uint32_t width, std::uint32_t height)
    {
        m_size = slint::PhysicalSize({width, height});
        window().dispatch_scale_factor_change_event(1.0F);
        window().dispatch_resize_event(
            slint::LogicalSize({static_cast<float>(width), static_cast<float>(height)}));
    }

    void draw(Surface& surface, bool translucent, const Surface::Matte& matte)
    {
        surface.resize(m_size.width, m_size.height);
        surface.paint(
            [this](std::span<slint::Rgb8Pixel> buffer, std::size_t stride) {
                [[maybe_unused]] const auto region = m_renderer.render(buffer, stride);
            },
            translucent, matte);
    }

private:
    slint::platform::SoftwareRenderer m_renderer{
        slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    slint::PhysicalSize m_size{slint::Size<std::uint32_t>{1, 1}};
};

class HeadlessPlatform final : public slint::platform::Platform
{
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override
    {
        auto window = std::make_unique<HeadlessWindow>();
        last = window.get();
        return window;
    }

    static inline HeadlessWindow* last = nullptr;
};

void ensurePlatform()
{
    static const bool once = [] {
        slint::platform::set_platform(std::make_unique<HeadlessPlatform>());
        return true;
    }();
    (void)once;
}

slint::ComponentHandle<slint::interpreter::ComponentInstance> compile(const std::string& source)
{
    ensurePlatform();
    slint::interpreter::ComponentCompiler compiler;
    const auto definition = compiler.build_from_source(source, "test.slint");
    std::string problems;
    for (const auto& diagnostic : compiler.diagnostics())
    {
        problems += std::format("{}:{} {}\n", diagnostic.line, diagnostic.column,
                                std::string_view(diagnostic.message));
    }
    EXPECT_TRUE(definition.has_value()) << problems << source;
    if (!definition)
    {
        std::abort(); // the tests below cannot go on
    }
    return definition->create();
}

std::uint32_t alphaOf(std::uint32_t pixel)
{
    return pixel >> 24;
}

Surface::Matte matteOf(const slint::ComponentHandle<slint::interpreter::ComponentInstance>& instance,
                       const std::string& property = "bar-matte")
{
    return [instance, property](bool white) {
        const auto color =
            white ? slint::Color::from_rgb_uint8(255, 255, 255) : slint::Color::from_rgb_uint8(0, 0, 0);
        instance->set_property(property, Value(slint::Brush(color)));
    };
}

TEST(BarSurface, RecoversCoverageFromTwoPasses)
{
    auto instance = compile(R"(
export component Test inherits Window {
    in property <brush> bar-matte: #000000;
    background: root.bar-matte;
    Rectangle { x: 0px; y: 0px; width: 10px; height: 10px; background: #ff0000; }
    Rectangle { x: 10px; y: 0px; width: 10px; height: 10px; background: #0000ff80; }
}
)");
    HeadlessPlatform::last->resize(30, 10);
    Surface surface;
    HeadlessPlatform::last->draw(surface, true, matteOf(instance));
    const auto pixels = surface.pixels();
    const std::uint32_t opaque = pixels[5 * 30 + 5];
    const std::uint32_t half = pixels[5 * 30 + 15];
    const std::uint32_t empty = pixels[5 * 30 + 25];
    EXPECT_EQ(alphaOf(opaque), 255u);
    EXPECT_EQ((opaque >> 16) & 0xFF, 255u); // red
    EXPECT_NEAR(static_cast<int>(alphaOf(half)), 128, 2);
    EXPECT_NEAR(static_cast<int>(half & 0xFF), 128, 2); // blue, premultiplied
    EXPECT_EQ(alphaOf(empty), 0u);
    EXPECT_EQ(empty, 0u);

    // Opaque mode: one pass, everything alpha 255.
    HeadlessPlatform::last->draw(surface, false, matteOf(instance));
    EXPECT_EQ(alphaOf(surface.pixels()[5 * 30 + 25]), 255u);
}

TEST(BarSurface, GeneratedBarsCompileAndDraw)
{
    for (const auto position : {rmw::domain::BarPosition::Bottom, rmw::domain::BarPosition::Left})
    {
        rmw::domain::BarConfig config;
        config.position = position;
        config.left = {{"start", {}}, {"desktops", {}}, {"cpu", {}}, {"volume", {}}, {"notifications", {}}};
        config.center = {{"media", {}}, {"clock", {}}};
        config.right = {{"windows", {}}, {"tray", {}}, {"clock", {}}, {"no-such-widget", {}}};
        const auto generated =
            rmw::bar::generateBarSource(config, rmw::barapp::builtinKinds(), rmw::barapp::builtinSources());
        EXPECT_EQ(generated.unknown.size(), 1u);
        auto instance = compile(generated.source);
        instance->set_property("bar-background",
                               Value(slint::Brush(slint::Color::from_rgb_uint8(32, 32, 32))));
        instance->set_property("w6-text", Value(slint::SharedString("12:34")));
        instance->set_property("w5-title", Value(slint::SharedString("A song \u2014 An artist")));
        instance->set_property("w5-shown", Value(true));
        // Two windows in the list (what the provider sets).
        auto windows = std::make_shared<slint::VectorModel<Value>>();
        for (int i = 0; i < 2; ++i)
        {
            windows->push_back(
                Value(slint::interpreter::Struct{{"id", Value(static_cast<double>(i))},
                                                 {"title", Value(slint::SharedString("A window"))},
                                                 {"icon", Value(slint::Image())},
                                                 {"active", Value(i == 0)}}));
        }
        instance->set_property("w7-windows", Value(std::shared_ptr<slint::Model<Value>>(windows)));
        // Two tray icons shown and three behind the chevron (what TrayWidget sets).
        auto icons = std::make_shared<slint::VectorModel<Value>>();
        for (int i = 0; i < 2; ++i)
        {
            icons->push_back(Value(slint::interpreter::Struct{{"id", Value(static_cast<double>(i))},
                                                              {"icon", Value(slint::Image())}}));
        }
        EXPECT_TRUE(instance->set_property("w8-icons", Value(std::shared_ptr<slint::Model<Value>>(icons))));
        EXPECT_TRUE(instance->set_property("w8-hidden-count", Value(3.0)));
        const bool horizontal = position == rmw::domain::BarPosition::Bottom;
        HeadlessPlatform::last->resize(horizontal ? 800 : 48, horizontal ? 48 : 600);
        Surface surface;
        HeadlessPlatform::last->draw(surface, true, matteOf(instance));
        // Something other than the background was drawn (the start mark, the clock text).
        const auto pixels = surface.pixels();
        const std::uint32_t background = pixels[0];
        EXPECT_EQ(alphaOf(background), 255u);
        EXPECT_TRUE(std::ranges::any_of(pixels, [&](std::uint32_t p) { return p != background; }));
    }
}

TEST(BarSurface, DockSpacerSeparatorAndTextDrawAndAutoLengthFollowsThem)
{
    // M10: the new widgets compile and draw, across and along; a bar of length "auto" knows how
    // long it wants to be, and grows with what it shows.
    for (const auto position : {rmw::domain::BarPosition::Bottom, rmw::domain::BarPosition::Left})
    {
        rmw::domain::BarConfig config;
        config.position = position;
        config.length = {true, 100.0, true};
        config.center = {{"start", {}}, {"dock", {}}, {"separator", {}}, {"spacer", {}}, {"text", {}}};
        const auto generated =
            rmw::bar::generateBarSource(config, rmw::barapp::builtinKinds(), rmw::barapp::builtinSources());
        EXPECT_TRUE(generated.unknown.empty());
        auto instance = compile(generated.source);
        const auto apps = [](int count) {
            auto model = std::make_shared<slint::VectorModel<Value>>();
            for (int i = 0; i < count; ++i)
            {
                model->push_back(Value(slint::interpreter::Struct{{"id", Value(static_cast<double>(i))},
                                                                  {"title", Value(slint::SharedString("An app"))},
                                                                  {"icon", Value(slint::Image())},
                                                                  {"running", Value(i % 2 == 0)},
                                                                  {"active", Value(i == 0)},
                                                                  {"count", Value(i == 0 ? 2.0 : 1.0)}}));
            }
            return Value(std::shared_ptr<slint::Model<Value>>(model));
        };
        ASSERT_TRUE(instance->set_property("w1-apps", apps(2)));
        ASSERT_TRUE(instance->set_property("w4-text", Value(slint::SharedString("Miku"))));
        ASSERT_TRUE(instance->set_property("w4-glyph", Value(slint::SharedString("\xEE\xA3\x96")))); // E8D6
        instance->set_property("bar-radius", Value(16.0));
        const bool horizontal = position == rmw::domain::BarPosition::Bottom;
        HeadlessPlatform::last->resize(horizontal ? 700 : 60, horizontal ? 60 : 700);
        Surface surface;
        HeadlessPlatform::last->draw(surface, true, matteOf(instance));
        const double two = instance->get_property("content-length")->to_number().value();
        ASSERT_TRUE(instance->set_property("w1-apps", apps(6)));
        HeadlessPlatform::last->draw(surface, true, matteOf(instance));
        const double six = instance->get_property("content-length")->to_number().value();
        EXPECT_GT(two, 150.0);
        EXPECT_GT(six, two + 150.0) << "four more apps, about 50 px each";
        const auto pixels = surface.pixels();
        EXPECT_TRUE(std::ranges::any_of(pixels, [&](std::uint32_t p) { return p != pixels[0]; }));
    }
}

TEST(BarSurface, TheStartMenuCompilesAndDraws)
{
    // M11: the menu as it opens, then while searching, then with every app and the power list.
    auto instance = compile(std::string(rmw::barapp::startMenuSource()));
    auto apps = std::make_shared<slint::VectorModel<Value>>();
    for (int i = 0; i < 40; ++i)
    {
        apps->push_back(Value(slint::interpreter::Struct{{"id", Value(slint::SharedString(std::format("app{}", i)))},
                                                         {"name", Value(slint::SharedString(std::format("App {}", i)))},
                                                         {"icon", Value(slint::Image())}}));
    }
    const Value model{std::shared_ptr<slint::Model<Value>>(apps)};
    ASSERT_TRUE(instance->set_property("pinned", model));
    ASSERT_TRUE(instance->set_property("apps", model));
    ASSERT_TRUE(instance->set_property("user", Value(slint::SharedString("Ana"))));
    ASSERT_TRUE(instance->set_property("open", Value(true)));
    HeadlessPlatform::last->resize(640, 700);
    Surface surface;
    for (const auto& [property, value] : std::vector<std::pair<std::string, Value>>{
             {"query", Value(slint::SharedString())},
             {"query", Value(slint::SharedString("App"))},
             {"all-apps", Value(true)},
             {"power-open", Value(true)}})
    {
        ASSERT_TRUE(instance->set_property(property, value)) << property;
        if (property == "query")
        {
            ASSERT_TRUE(instance->set_property("results", model));
        }
        HeadlessPlatform::last->draw(surface, true, matteOf(instance, "menu-matte"));
        const auto pixels = surface.pixels();
        EXPECT_TRUE(std::ranges::any_of(pixels, [&](std::uint32_t p) { return p != pixels[0]; })) << property;
    }
}

TEST(BarSurface, TheStartZoneNeverRunsIntoTheCenter)
{
    // Found on a real desktop: many windows pushed the list under the centered clock.
    rmw::domain::BarConfig config;
    config.left = {{"start", {}}, {"windows", {}}};
    config.center = {{"clock", {}}};
    config.right = {{"clock", {}}};
    const auto generated =
        rmw::bar::generateBarSource(config, rmw::barapp::builtinKinds(), rmw::barapp::builtinSources());
    auto instance = compile(generated.source);
    auto windows = std::make_shared<slint::VectorModel<Value>>();
    for (int i = 0; i < 12; ++i)
    {
        windows->push_back(
            Value(slint::interpreter::Struct{{"id", Value(static_cast<double>(i))},
                                             {"title", Value(slint::SharedString("A long window title"))},
                                             {"icon", Value(slint::Image())},
                                             {"active", Value(false)}}));
    }
    instance->set_property("w1-windows", Value(std::shared_ptr<slint::Model<Value>>(windows)));
    instance->set_property("w1-labels", Value(true));
    instance->set_property("w2-text", Value(slint::SharedString("12:34")));
    instance->set_property("w3-text", Value(slint::SharedString("05/10/2026")));
    HeadlessPlatform::last->resize(1200, 44);
    Surface surface;
    HeadlessPlatform::last->draw(surface, false, matteOf(instance));
    const auto startEnd = instance->get_property("layout-start-end")->to_number().value();
    const auto centerStart = instance->get_property("layout-center-start")->to_number().value();
    EXPECT_LE(startEnd, centerStart) << "the window list runs under the clock";
    EXPECT_GT(startEnd, 300.0); // and it still uses the room it has
}

TEST(PluginWidgetSource, WhatTheCheckerWritesCompiles)
{
    // The checker re-writes the source token by token: the result must still be Slint.
    const auto folder = std::filesystem::temp_directory_path() / "rmw-widget-source-test";
    std::filesystem::create_directories(folder / "icons");
    {
        std::ofstream(folder / "icons" / "play.svg")
            << R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16"/></svg>)";
    }
    constexpr std::string_view kWidget = R"slint(
import { Button } from "std-widgets.slint";
export struct Line { text: string, bold: bool }
component Icon inherits Image { width: 16px; height: 16px; }
export component NowPlaying inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <string> title: "?";
    in property <[Line]> lines;
    callback toggle();
    spacing: 6px;
    Icon { source: @image-url("icons/play.svg"); }
    Text { text: "\{root.title} (\{root.lines.length})"; color: root.foreground; }
    for line[i] in root.lines: Text { text: line.text; font-weight: line.bold ? 700 : 400; }
    for i in 3: Rectangle { width: 2px; background: #ff000080; }
    Button { text: "x"; clicked => { root.toggle(); } }
}
)slint";
    rmw::bar::WidgetSourceOptions options;
    options.prefix = "Rmwp1";
    options.component = "NowPlaying";
    options.resolveImage = [&folder](std::string_view path) -> rmw::Result<std::string> {
        return (folder / std::filesystem::path(path)).generic_string();
    };
    const auto checked = rmw::bar::checkWidgetSource(kWidget, "widget.slint", options);
    ASSERT_TRUE(checked.has_value()) << checked.error().message();
    std::string source = "import { Button } from \"std-widgets.slint\";\n" + checked->source;
    source += std::format("export component Test inherits Window {{ width: 300px; height: 40px; {} {{ title: "
                          "\"playing\"; lines: [{{ text: \"a\", bold: true }}]; }} }}\n",
                          checked->component);
    auto instance = compile(source);
    HeadlessPlatform::last->resize(300, 40);
    Surface surface;
    HeadlessPlatform::last->draw(surface, false, [](bool) {});
    std::filesystem::remove_all(folder);
}

TEST(PluginWidgetSource, EveryExampleWidgetCompilesInABar)
{
    // plugins/examples: what `rmw plugin validate` cannot check (it has no Slint) is checked here.
    const std::filesystem::path examples = std::filesystem::path(RMW_SOURCE_DIR) / "plugins" / "examples";
    std::vector<rmw::bar::PlanPlugin> plugins;
    rmw::domain::BarConfig config;
    for (const auto& entry : std::filesystem::directory_iterator(examples))
    {
        if (!std::filesystem::exists(entry.path() / "widget.slint"))
        {
            continue;
        }
        const auto toml = std::filesystem::path(entry.path() / "plugin.toml");
        rmw::domain::Diagnostics diagnostics;
        std::ifstream file(toml, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto manifest = rmw::domain::parsePluginManifest(text, diagnostics, {});
        ASSERT_TRUE(manifest.has_value()) << toml.string();
        plugins.push_back({manifest->id, std::filesystem::absolute(entry.path()), false});
        config.right.push_back({manifest->id, {}});
    }
    ASSERT_GE(plugins.size(), 6u);
    rmw::barapp::PluginWidgets widgets;
    widgets.load(plugins);
    ASSERT_EQ(widgets.all().size(), plugins.size()) << "some example widget did not load (see the log)";
    const auto kinds = widgets.kinds();
    const auto generated =
        rmw::bar::generateBarSource(config, kinds, widgets.sources(rmw::barapp::builtinSources()));
    EXPECT_TRUE(generated.unknown.empty());
    auto instance = compile(generated.source);
    HeadlessPlatform::last->resize(1600, 44);
    // Values as the host would send them, converted by the bar.
    for (const auto& slot : generated.slots)
    {
        const auto* widget = widgets.find(slot.kind->id);
        ASSERT_NE(widget, nullptr);
        for (const auto& property : widget->manifest.properties)
        {
            rmw::domain::Value value;
            using rmw::domain::WidgetType;
            if (!property.type.array && property.type.structName.empty())
            {
                switch (property.type.type)
                {
                case WidgetType::String:
                    value = rmw::domain::Value{std::string("text")};
                    break;
                case WidgetType::Int:
                    value = rmw::domain::Value{std::int64_t{3}};
                    break;
                case WidgetType::Float:
                case WidgetType::Length:
                    value = rmw::domain::Value{0.5};
                    break;
                case WidgetType::Bool:
                    value = rmw::domain::Value{true};
                    break;
                case WidgetType::Color:
                    value = rmw::domain::Value{std::string("#ff0000")};
                    break;
                case WidgetType::Image:
                    value = rmw::domain::Value{std::string()};
                    break;
                }
            }
            else
            {
                value = rmw::domain::Value{rmw::domain::Value::Array{}};
            }
            auto converted = widgets.convert(*widget, property.type, value);
            ASSERT_TRUE(converted.has_value()) << slot.kind->id << "." << property.name;
            EXPECT_TRUE(instance->set_property(std::format("{}-{}", slot.prefix, property.name), *converted))
                << slot.kind->id << "." << property.name;
        }
    }
    Surface surface;
    HeadlessPlatform::last->draw(surface, false, matteOf(instance));
}

TEST(PluginWidgetSource, SlintErrorsPointAtThePluginsOwnLine)
{
    // What the checks let through but Slint refuses: the bar tries the widget alone and reports
    // Slint's message at the line of widget.slint (BarApp::leaveOutBrokenWidgets).
    const auto folder = std::filesystem::temp_directory_path() / "rmw-broken-widget-test";
    std::filesystem::create_directories(folder);
    {
        std::ofstream(folder / "plugin.toml") << R"(schema = 1
[plugin]
id      = "io.github.test.broken"
name    = "Broken"
version = "1.0.0"
api     = "^1.0"
kind    = ["bar.widget"]
[widget]
ui        = "widget.slint"
component = "Broken"
[widget.properties]
text = "string"
)";
        std::ofstream(folder / "widget.slint") << R"(// line 1
/* a comment
   on two lines */
import { Button } from "std-widgets.slint";
export component Broken inherits Rectangle {
    in property <string> text;
    Text { text: root.missing; }
}
)";
    }
    rmw::barapp::PluginWidgets widgets;
    widgets.load({{"io.github.test.broken", folder, false}});
    ASSERT_EQ(widgets.all().size(), 1u);
    const auto& widget = *widgets.all().front();

    rmw::domain::BarConfig config;
    config.right.push_back({widget.id, {}});
    std::vector<rmw::bar::WidgetKind> kinds(rmw::barapp::builtinKinds().begin(),
                                            rmw::barapp::builtinKinds().end());
    kinds.push_back(widget.kind);
    const auto generated = rmw::bar::generateBarSource(
        config, kinds, rmw::barapp::PluginWidgets::sourcesWith(widget, rmw::barapp::builtinSources()));
    slint::interpreter::ComponentCompiler compiler;
    EXPECT_FALSE(compiler.build_from_source(generated.source, "bar.slint").has_value());
    const std::size_t before = rmw::barapp::PluginWidgets::lineBefore(generated.source, widget.id);
    ASSERT_GT(before, 0u);
    std::size_t errors = 0;
    for (const auto& diagnostic : compiler.diagnostics())
    {
        if (diagnostic.level == slint::interpreter::DiagnosticLevel::Error)
        {
            ++errors;
            EXPECT_EQ(diagnostic.line - before, 7u) << std::string_view(diagnostic.message);
        }
    }
    EXPECT_GE(errors, 1u);

    widgets.remove(widget.id);
    EXPECT_TRUE(widgets.all().empty());
    std::filesystem::remove_all(folder);
}

TEST(BarClock, FormatsSafely)
{
    EXPECT_TRUE(std::regex_match(rmw::barapp::formatClock("%H:%M"), std::regex(R"(\d\d:\d\d)")));
    EXPECT_TRUE(std::regex_match(rmw::barapp::formatClock("%d/%m/%Y"), std::regex(R"(\d\d/\d\d/\d{4})")));
    EXPECT_EQ(rmw::barapp::formatClock("100%% %Q %"), "100% %Q %");
    EXPECT_FALSE(rmw::barapp::formatClock("%A").empty()); // the user's language
}

} // namespace
