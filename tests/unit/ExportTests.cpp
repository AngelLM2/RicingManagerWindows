#include "rmw/domain/RiceParser.hpp"
#include "rmw/modules/Export.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using rmw::json::Json;
using rmw::modules::CapturedLook;
using rmw::modules::exportRice;
using rmw::modules::tomlString;

std::string extensionOf(std::string_view path)
{
    if (path.ends_with("TranscodedWallpaper"))
    {
        return "jpg"; // by content
    }
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? std::string() : std::string(path.substr(dot + 1));
}

CapturedLook sampleLook()
{
    CapturedLook look;
    look.appearance = {{"appsLight", false},
                       {"systemLight", false},
                       {"accent", "#7AA2F7"},
                       {"transparency", true},
                       {"accentOnTaskbar", true}};
    look.taskbar = {{"alignment", 0}, {"taskView", 0}, {"search", 1}, {"autohide", false}};
    look.cursors = {{"Arrow", {{"type", "expand"}, {"data", R"(%SystemRoot%\cursors\aero_arrow.cur)"}}},
                    {"Wait", {{"type", "expand"}, {"data", R"(C:\Packs\Neon\busy.ANI)"}}},
                    {"Help", nullptr},
                    {"Hand", {{"type", "string"}, {"data", R"(C:\x\link.png)"}}},
                    {"size", 48}};
    look.monitors = {{"", "AAA-1", "Main", true, 0, 0, 2560, 1440},
                     {"", "BBB-2", "Side", false, -1920, 0, 0, 1080}};
    look.wallpaper = {{"monitor:AAA-1", {{"file", R"(C:\Users\me\Pictures\city "night".jpg)"}}},
                      {"monitor:BBB-2", {{"file", ""}}},
                      {"position", 3},
                      {"background", "#1A1B26"},
                      {"slideshow", nullptr},
                      {"backgroundType", 0}};
    return look;
}

TEST(Export, RoundTripsThroughTheParser)
{
    const auto exported = exportRice("My desk", sampleLook(), extensionOf);
    ASSERT_TRUE(exported.has_value());
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << rice.error().toString() << "\n" << exported->toml;
    for (const auto& d : diagnostics.all())
    {
        ADD_FAILURE() << d.toString("rice.toml") << "\n" << exported->toml;
    }

    EXPECT_EQ(rice->meta.name, "My desk");
    EXPECT_EQ(rice->appearance->mode, rmw::domain::ThemeMode::Dark);
    EXPECT_EQ(rice->appearance->accent->toHex(), "#7AA2F7");
    EXPECT_EQ(rice->taskbar->alignment, rmw::domain::TaskbarAlignment::Left);
    EXPECT_EQ(rice->taskbar->search, rmw::domain::TaskbarSearch::Icon);
    ASSERT_EQ(rice->cursors->map.size(), 2u); // the .png "cursor" is left out
    const auto busy = std::ranges::find(rice->cursors->map, rmw::domain::CursorRole::Busy,
                                        &rmw::domain::CursorAssignment::role);
    ASSERT_NE(busy, rice->cursors->map.end());
    EXPECT_EQ(busy->file, "busy.ani");
    EXPECT_EQ(rice->cursors->size, 48u);
    ASSERT_EQ(rice->wallpaper->monitors.size(), 2u);
    EXPECT_EQ(rice->wallpaper->monitors[0].match.edidKey, "AAA-1");
    EXPECT_EQ(rice->wallpaper->monitors[0].fit, rmw::domain::WallpaperFit::Fit);
    EXPECT_EQ(rice->wallpaper->monitors[1].kind, rmw::domain::WallpaperKind::Color);

    ASSERT_EQ(exported->files.size(), 3u);
    EXPECT_EQ(exported->files[0].relative, "assets/cursors/normal.cur");
    EXPECT_EQ(exported->files[2].relative, "assets/wallpaper/1.jpg");
    EXPECT_FALSE(exported->notes.empty()); // the skipped .png
}

TEST(Export, SlideshowAndSpan)
{
    CapturedLook look;
    look.monitors = sampleLook().monitors;
    look.wallpaper = {
        {"slideshow",
         {{"items", {R"(C:\p\a.jpg)", R"(C:\p\b.png)"}}, {"shuffle", true}, {"intervalMs", 60000}}},
        {"position", 4}};
    auto exported = exportRice("Slides", look, extensionOf);
    ASSERT_TRUE(exported.has_value());
    rmw::domain::Diagnostics diagnostics;
    auto rice = rmw::domain::parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    EXPECT_EQ(rice->wallpaper->monitors[0].kind, rmw::domain::WallpaperKind::Slideshow);
    EXPECT_EQ(rice->wallpaper->monitors[0].intervalSeconds, 60u);
    EXPECT_EQ(exported->files.size(), 2u);

    look.wallpaper = {
        {"monitor:AAA-1",
         {{"file", R"(C:\Users\me\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)"}}},
        {"monitor:BBB-2",
         {{"file", R"(C:\Users\me\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper)"}}},
        {"position", 5}};
    exported = exportRice("Wide", look, extensionOf);
    ASSERT_TRUE(exported.has_value());
    rice = rmw::domain::parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    EXPECT_EQ(rice->wallpaper->layout, rmw::domain::WallpaperLayout::Span);
    EXPECT_EQ(exported->files.size(), 1u);
}

TEST(Export, TomlStringsEscapeEverything)
{
    EXPECT_EQ(tomlString(R"(a"b\c)"), R"("a\"b\\c")");
    EXPECT_EQ(tomlString(std::string("x\ny\x01")), "\"x\\ny\\u0001\"");
    EXPECT_EQ(tomlString("夜景"), "\"夜景\"");
    EXPECT_FALSE(exportRice("", CapturedLook{}, extensionOf).has_value());
}

} // namespace
