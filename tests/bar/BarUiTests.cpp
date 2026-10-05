#include "Surface.hpp"
#include "Widgets.hpp"

#include "rmw/bar/Source.hpp"

#include <gtest/gtest.h>
#include <slint-interpreter.h>
#include <slint-platform.h>

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

Surface::Matte matteOf(const slint::ComponentHandle<slint::interpreter::ComponentInstance>& instance)
{
    return [instance](bool white) {
        const auto color =
            white ? slint::Color::from_rgb_uint8(255, 255, 255) : slint::Color::from_rgb_uint8(0, 0, 0);
        instance->set_property("bar-matte", Value(slint::Brush(color)));
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
        config.right = {{"windows", {}}, {"clock", {}}, {"no-such-widget", {}}};
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

TEST(BarClock, FormatsSafely)
{
    EXPECT_TRUE(std::regex_match(rmw::barapp::formatClock("%H:%M"), std::regex(R"(\d\d:\d\d)")));
    EXPECT_TRUE(std::regex_match(rmw::barapp::formatClock("%d/%m/%Y"), std::regex(R"(\d\d/\d\d/\d{4})")));
    EXPECT_EQ(rmw::barapp::formatClock("100%% %Q %"), "100% %Q %");
    EXPECT_FALSE(rmw::barapp::formatClock("%A").empty()); // the user's language
}

} // namespace
