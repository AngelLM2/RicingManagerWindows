#include "rmw/bar/Bridge.hpp"
#include "rmw/bar/Plan.hpp"
#include "rmw/domain/Images.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/plugins/HostPlan.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace
{

using namespace rmw;

TEST(PluginBridge, MessagesRoundTrip)
{
    namespace bridge = bar::bridge;
    const domain::Value config{domain::Value::Table{{"city", domain::Value{std::string("Lisboa")}}}};
    const auto instances =
        bridge::decode(bridge::encode(bridge::Instances{{{"0/w3", "io.github.x.weather", config}}}));
    ASSERT_TRUE(instances.has_value()) << instances.error().message();
    const auto& list = std::get<bridge::Instances>(*instances).list;
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].plugin, "io.github.x.weather");
    EXPECT_EQ(list[0].config, config);

    const auto set = bridge::decode(bridge::encode(bridge::Set{
        "0/w3", domain::Value{domain::Value::Table{{"text", domain::Value{std::string("21°")}}}}}));
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(std::get<bridge::Set>(*set).key, "0/w3");

    const auto event = bridge::decode(bridge::encode(
        bridge::Event{"0/w3", "picked", {domain::Value{std::int64_t{2}}, domain::Value{true}}}));
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(std::get<bridge::Event>(*event).arguments.size(), 2u);

    const auto noArgs = bridge::decode(bridge::encode(bridge::Event{"0/w3", "clicked", {}}));
    ASSERT_TRUE(noArgs.has_value()) << noArgs.error().message();
}

TEST(PluginBridge, BadMessagesAreRefused)
{
    for (const std::string_view text : {
             R"({"t":"nope"})",
             R"({"t":"set","key":"k","props":[1,2]})",
             R"({"t":"set","props":{}})",
             R"({"t":"instances","list":[{"key":"k","plugin":"not an id","config":{}}]})",
             R"({"t":"event","key":"k","callback":"c","args":[1,2,3,4,5,6,7,8,9]})",
             R"([1,2,3])",
             R"({"t":"set","key":"k","props":)",
         })
    {
        EXPECT_FALSE(bar::bridge::decode(text).has_value()) << text;
    }
}

TEST(PluginHostPlan, RoundTripAndChecks)
{
    plugins::HostPlan plan;
    plugins::HostPlugin plugin;
    plugin.id = "io.github.x.weather";
    plugin.folder = LR"(C:\Users\x\AppData\Local\RicingManagerWindows\plugins\io.github.x.weather)";
    plugin.granted = {{"time"}, {"bar.toggle"}, {"api.open-meteo.com"}, true};
    plugin.config = domain::Value{domain::Value::Table{{"city", domain::Value{std::string("Lisboa")}}}};
    plugin.dev = true;
    plan.plugins.push_back(plugin);
    const auto parsed = plugins::hostPlanFromJson(plugins::toJson(plan));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    ASSERT_EQ(parsed->plugins.size(), 1u);
    EXPECT_EQ(parsed->plugins[0], plugin);

    for (
        const std::string_view text : {
            R"({"version":2,"plugins":[]})",
            R"({"version":1,"plugins":[{"id":"bad id","folder":"C:\\x"}]})",
            R"({"version":1,"plugins":[{"id":"io.a.b","folder":"relative\\x"}]})",
            R"({"version":1,"plugins":[{"id":"io.a.b","folder":"C:\\x","granted":{"providers":["files"]}}]})",
            R"({"version":1,"plugins":[{"id":"io.a.b","folder":"C:\\x","granted":{"network":["127.0.0.1"]}}]})",
            R"({"version":1,"plugins":[{"id":"io.a.b","folder":"C:\\x"},{"id":"io.a.b","folder":"C:\\y"}]})",
        })
    {
        EXPECT_FALSE(plugins::hostPlanFromJson(text).has_value()) << text;
    }
}

TEST(PluginBarPlan, PluginFoldersAndSkinColorsTravel)
{
    domain::Diagnostics diagnostics;
    auto rice = domain::parseRice(R"(schema = 1
[meta]
name = "r"
[[bar]]
foreground = "#ffffff"
left = ["start"]
)",
                                  diagnostics);
    ASSERT_TRUE(rice.has_value());
    // A skin's palette name the rice does not define: left out, not an error.
    rice->bars[0].background.tint = domain::ColorRef{std::string("base")};
    auto plan = bar::planFromRice(*rice);
    ASSERT_TRUE(plan.has_value()) << plan.error().message();
    EXPECT_FALSE(plan->bars[0].background.tint.has_value());
    plan->plugins.push_back({"io.github.x.weather", LR"(C:\plugins\weather)", true});
    const auto back = bar::planFromJson(bar::toJson(*plan));
    ASSERT_TRUE(back.has_value()) << back.error().message();
    ASSERT_EQ(back->plugins.size(), 1u);
    EXPECT_EQ(back->plugins[0], plan->plugins[0]);
}

TEST(PluginImages, HeaderSizeWithoutDecoding)
{
    // A PNG that claims 40000 x 40000 in 33 bytes: the size check sees it before any decoder.
    const std::array<unsigned char, 24> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0,    13,
                                               'I',  'H', 'D', 'R', 0,    0,    0x9C, 0x40, 0, 0, 0x9C, 0x40};
    const auto size = domain::rasterImageSize(std::as_bytes(std::span(png)));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->width, 40000u);
    EXPECT_EQ(size->height, 40000u);

    // JPEG: SOI, an APP0 segment, then SOF0 with height 480 and width 640.
    const std::array<unsigned char, 25> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF,
                                                0xC0, 0x00, 0x11, 0x08, 0x01, 0xE0, 0x02, 0x80, 0x03,
                                                0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03};
    const auto photo = domain::rasterImageSize(std::as_bytes(std::span(jpeg)));
    ASSERT_TRUE(photo.has_value());
    EXPECT_EQ(photo->width, 640u);
    EXPECT_EQ(photo->height, 480u);

    const std::array<unsigned char, 4> junk = {'G', 'I', 'F', '8'};
    EXPECT_FALSE(domain::rasterImageSize(std::as_bytes(std::span(junk))).has_value());
}

TEST(PluginActions, NamesSplit)
{
    const auto action = domain::parsePluginActionName("io.github.x.focus-mode:toggle");
    ASSERT_TRUE(action.has_value());
    EXPECT_EQ(action->plugin, "io.github.x.focus-mode");
    EXPECT_EQ(action->action, "toggle");
    for (const std::string_view name : {"bar.toggle", "io.github.x.focus:", ":toggle", "single:toggle",
                                        "io.github.x.focus:Toggle", "io.github.x.focus:a:b"})
    {
        EXPECT_FALSE(domain::parsePluginActionName(name).has_value()) << name;
    }
}

} // namespace
