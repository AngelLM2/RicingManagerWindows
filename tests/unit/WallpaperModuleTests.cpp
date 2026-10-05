#include "FakeDisplay.hpp"
#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/engine/RiceEngine.hpp"
#include "rmw/modules/Wallpaper.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>

namespace
{

using namespace rmw::modules;
using rmw::engine::Json;
using rmw::ports::WallpaperPosition;
using rmw::test::FakeDisplayTopology;
using rmw::test::FakeRegistry;
using rmw::test::FakeWallpaperApi;

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

rmw::domain::MonitorWallpaper rule(const char* match, rmw::domain::WallpaperKind kind,
                                   std::string source = {})
{
    rmw::domain::MonitorWallpaper wallpaper;
    wallpaper.match = rmw::domain::MonitorSelector::parse(match).value();
    wallpaper.kind = kind;
    wallpaper.source = std::move(source);
    return wallpaper;
}

class WallpaperTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::create_directories(package() / "assets" / "slides");
        std::ofstream(package() / "assets" / "a.jpg", std::ios::binary) << "AAAA";
        std::ofstream(package() / "assets" / "b.jpg", std::ios::binary) << "BBBB";
        std::ofstream(package() / "assets" / "slides" / "1.jpg", std::ios::binary) << "S1";
        std::ofstream(package() / "assets" / "slides" / "2.png", std::ios::binary) << "S2";
        std::ofstream(package() / "assets" / "slides" / "notes.txt", std::ios::binary) << "x";
        m_rice.meta.name = "Walls";
        m_rice.wallpaper = rmw::domain::WallpaperConfig{};
    }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    rmw::Result<Json> desired()
    {
        rmw::engine::StoreAssetSink sink(m_store, package());
        return WallpaperLogic(m_topology).desiredFromRice(m_rice, sink);
    }

    rmw::test::TempDir m_dir;
    rmw::engine::ContentStore m_store{m_dir.path() / "store"};
    FakeDisplayTopology m_topology;
    rmw::domain::Rice m_rice;
};

TEST_F(WallpaperTest, RulesBecomeOneFieldPerMonitor)
{
    auto primary = rule("primary", rmw::domain::WallpaperKind::Image, "assets/a.jpg");
    primary.fit = rmw::domain::WallpaperFit::Center;
    m_rice.wallpaper->monitors = {primary, rule("all", rmw::domain::WallpaperKind::Image, "assets/b.jpg")};
    m_rice.wallpaper->background = rmw::domain::Color{0x1a, 0x1b, 0x26, 0xff};

    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    const std::filesystem::path main = (*fields)["monitor:AAA-1"]["file"].get<std::string>();
    const std::filesystem::path side = (*fields)["monitor:BBB-2"]["file"].get<std::string>();
    EXPECT_EQ(main.parent_path(), m_dir.path() / "store");
    EXPECT_NE(main, side);
    EXPECT_EQ((*fields)["position"], static_cast<std::uint32_t>(WallpaperPosition::Center));
    EXPECT_EQ((*fields)["background"], "#1A1B26");
    EXPECT_EQ((*fields)["slideshow"], nullptr);
    EXPECT_EQ((*fields)["backgroundType"], 0);
}

TEST_F(WallpaperTest, OneColorEverywhereNeedsNoImage)
{
    auto color = rule("all", rmw::domain::WallpaperKind::Color);
    color.color = rmw::domain::Color{0x10, 0x20, 0x30, 0xff};
    m_rice.wallpaper->monitors = {color};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value());
    EXPECT_EQ((*fields)["background"], "#102030");
    EXPECT_EQ((*fields)["monitor:AAA-1"]["file"], "");
    EXPECT_EQ((*fields)["backgroundType"], 1);
    EXPECT_FALSE(std::filesystem::exists(m_dir.path() / "store"));
}

TEST_F(WallpaperTest, ColorNextToAnImageBecomesASmallSolidImage)
{
    auto color = rule("left", rmw::domain::WallpaperKind::Color);
    color.color = rmw::domain::Color{0xff, 0, 0, 0xff};
    m_rice.wallpaper->monitors = {color, rule("all", rmw::domain::WallpaperKind::Image, "assets/a.jpg")};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    const std::filesystem::path solid = (*fields)["monitor:BBB-2"]["file"].get<std::string>();
    EXPECT_EQ(solid.extension(), ".bmp");
    // 1920x1080 -> 16x9 pixels: 54 bytes of headers + 9 rows of 48 bytes.
    EXPECT_EQ(std::filesystem::file_size(solid), 54u + 9u * 48u);
}

TEST_F(WallpaperTest, SlideshowFolderBecomesTheNativeSlideshow)
{
    auto slides = rule("all", rmw::domain::WallpaperKind::Slideshow, "assets/slides");
    slides.intervalSeconds = 600;
    slides.shuffle = true;
    m_rice.wallpaper->monitors = {slides};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    const Json& slideshow = (*fields)["slideshow"];
    ASSERT_EQ(slideshow["items"].size(), 2u); // notes.txt is not an image
    EXPECT_EQ(slideshow["intervalMs"], 600'000);
    EXPECT_TRUE(slideshow["shuffle"].get<bool>());
    EXPECT_FALSE(fields->contains("monitor:AAA-1"));

    rmw::engine::StoreAssetSink sink(m_store, package());
    const auto monitors = m_topology.monitors().value();
    EXPECT_TRUE(wallpaperRotation(*m_rice.wallpaper, monitors, sink).value().empty()); // Windows does it
}

TEST_F(WallpaperTest, ASlideshowNextToSomethingElseIsTurnedByTheCore)
{
    auto slides = rule("primary", rmw::domain::WallpaperKind::Slideshow, "assets/slides");
    slides.intervalSeconds = 90;
    m_rice.wallpaper->monitors = {slides, rule("all", rmw::domain::WallpaperKind::Image, "assets/a.jpg")};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    EXPECT_EQ((*fields)["slideshow"], nullptr);
    EXPECT_EQ((*fields)["backgroundType"], 0);

    rmw::engine::StoreAssetSink sink(m_store, package());
    const auto monitors = m_topology.monitors().value();
    const auto plan = wallpaperRotation(*m_rice.wallpaper, monitors, sink);
    ASSERT_TRUE(plan.has_value()) << plan.error().toString();
    ASSERT_EQ(plan->size(), 1u); // only the primary monitor turns pages
    const Json& primary = (*plan)["monitor:AAA-1"];
    ASSERT_EQ(primary["items"].size(), 2u);
    EXPECT_EQ(primary["intervalMs"], 90'000);
    EXPECT_FALSE(primary["shuffle"].get<bool>());
    // The first page is what the apply shows now.
    EXPECT_EQ((*fields)["monitor:AAA-1"]["file"], primary["items"][0]);

    // A folder without images is still an error.
    std::filesystem::create_directories(package() / "assets" / "empty");
    m_rice.wallpaper->monitors[0].source = "assets/empty";
    EXPECT_FALSE(desired().has_value());
}

TEST(WallpaperPages, NextPageWrapsAndShuffleNeverRepeats)
{
    EXPECT_EQ(nextPage(3, 0, false, 0), 1u);
    EXPECT_EQ(nextPage(3, 2, false, 0), 0u); // wraps
    EXPECT_EQ(nextPage(3, 3, false, 7), 0u); // not one of the pages: start over
    EXPECT_EQ(nextPage(0, 0, true, 7), 0u);
    for (std::uint64_t random = 0; random < 50; ++random)
    {
        for (std::size_t current = 0; current < 4; ++current)
        {
            const auto next = nextPage(4, current, true, random);
            EXPECT_LT(next, 4u);
            EXPECT_NE(next, current);
        }
    }
}

TEST_F(WallpaperTest, SpanAndVideoFallbacks)
{
    m_rice.wallpaper->layout = rmw::domain::WallpaperLayout::Span;
    m_rice.wallpaper->spanSource = "assets/a.jpg";
    auto span = desired();
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ((*span)["position"], static_cast<std::uint32_t>(WallpaperPosition::Span));
    EXPECT_EQ((*span)["monitor:AAA-1"], (*span)["monitor:BBB-2"]);

    m_rice.wallpaper->layout = rmw::domain::WallpaperLayout::PerMonitor;
    auto video = rule("primary", rmw::domain::WallpaperKind::Video, "assets/city.mp4");
    video.fallbackImage = "assets/b.jpg";
    m_rice.wallpaper->monitors = {video, rule("left", rmw::domain::WallpaperKind::Video, "assets/city.mp4")};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    EXPECT_TRUE(fields->contains("monitor:AAA-1"));
    EXPECT_FALSE(fields->contains("monitor:BBB-2")); // no fallback: left alone
}

TEST(WallpaperSystem, CaptureCopiesWhatIsShownAndFlagsSpotlight)
{
    const rmw::test::TempDir dir;
    std::ofstream(dir.path() / "TranscodedWallpaper", std::ios::binary) << "BMxx";
    FakeDisplayTopology topology;
    FakeWallpaperApi api;
    FakeRegistry registry;
    api.files[R"(\\?\DISPLAY#AAA#1)"] = utf8(dir.path() / "deleted.jpg"); // gone
    api.files[R"(\\?\DISPLAY#BBB#2)"] = R"(C:\Windows\Web\Wallpaper\Windows\img0.jpg)";
    api.transcoded = utf8(dir.path() / "TranscodedWallpaper");
    registry.setDword(R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers)", "BackgroundType",
                      3);
    WallpaperSystem system(topology, api, registry);

    const auto captured = system.capture();
    ASSERT_TRUE(captured.has_value()) << captured.error().toString();
    EXPECT_EQ(captured->state["monitor:AAA-1"], (Json{{"file", api.transcoded}, {"ext", "bmp"}}));
    EXPECT_TRUE(captured->state["monitor:BBB-2"]["keep"].get<bool>());
    EXPECT_EQ(captured->fidelity, rmw::engine::Fidelity::Lossy);
    EXPECT_EQ(captured->state["slideshow"], nullptr);

    // internalize: the transcoded copy goes to the store with its real type; Windows files stay.
    rmw::engine::ContentStore store(dir.path() / "store");
    rmw::engine::StoreAssetSink sink(store, {});
    Json state = captured->state;
    ASSERT_TRUE(WallpaperLogic(topology).internalize(state, sink));
    const std::filesystem::path kept = state["monitor:AAA-1"]["file"].get<std::string>();
    EXPECT_EQ(kept.parent_path(), dir.path() / "store");
    EXPECT_EQ(kept.extension(), ".bmp");
    EXPECT_FALSE(state["monitor:AAA-1"].contains("ext"));
    EXPECT_EQ(state["monitor:BBB-2"], (Json{{"file", R"(C:\Windows\Web\Wallpaper\Windows\img0.jpg)"}}));
}

TEST(WallpaperSystem, ApplyMapsIdentityKeysToDevicePaths)
{
    FakeDisplayTopology topology;
    FakeWallpaperApi api;
    FakeRegistry registry;
    WallpaperSystem system(topology, api, registry);

    ASSERT_TRUE(system.apply({{"monitor:BBB-2", {{"file", R"(C:\s\b.jpg)"}}},
                              {"monitor:GONE-9", {{"file", R"(C:\s\x.jpg)"}}}, // disconnected: skipped
                              {"position", 3},
                              {"background", "#FF8800"},
                              {"backgroundType", 0}}));
    EXPECT_EQ(api.files[R"(\\?\DISPLAY#BBB#2)"], R"(C:\s\b.jpg)");
    EXPECT_EQ(api.setCalls, 1);
    EXPECT_EQ(api.currentPosition, WallpaperPosition::Fit);
    EXPECT_EQ(api.background, 0xFF8800u);

    ASSERT_TRUE(system.apply(
        {{"slideshow", {{"items", {"a.jpg", "b.jpg"}}, {"shuffle", true}, {"intervalMs", 60000}}}}));
    EXPECT_TRUE(api.slideshowState.active);
    EXPECT_EQ(api.slideshowState.items.size(), 2u);

    EXPECT_FALSE(system.apply({{"position", 9}}).has_value());
    EXPECT_FALSE(system.apply({{"background", "red"}}).has_value());
    EXPECT_FALSE(system.apply({{"monitor:AAA-1", "x.jpg"}}).has_value());
}

TEST_F(WallpaperTest, ApplyThenRestoreOriginalUnderTheEngine)
{
    std::ofstream(m_dir.path() / "mine.jpg", std::ios::binary) << "MINE";
    FakeWallpaperApi api;
    FakeRegistry registry;
    api.files[R"(\\?\DISPLAY#AAA#1)"] = utf8(m_dir.path() / "mine.jpg");
    api.files[R"(\\?\DISPLAY#BBB#2)"] = R"(C:\Windows\Web\Wallpaper\Windows\img0.jpg)";
    api.background = 0x123456;
    const auto originalSide = api.files[R"(\\?\DISPLAY#BBB#2)"];

    const WallpaperLogic logic(m_topology);
    WallpaperSystem system(m_topology, api, registry);
    struct Appearance final : rmw::engine::IModuleLogic, rmw::engine::IModuleSystem
    {
        std::string_view id() const noexcept override { return "appearance"; }
        rmw::Result<Json> desiredFromRice(const rmw::domain::Rice&, rmw::engine::IAssetSink&) const override
        {
            return Json::object();
        }
        rmw::Result<rmw::engine::CaptureResult> capture() override { return rmw::engine::CaptureResult{}; }
        rmw::Result<void> apply(const Json&) override { return {}; }
    };
    Appearance appearance;
    rmw::engine::ModuleCatalog catalog;
    ASSERT_TRUE(catalog.add(appearance));
    ASSERT_TRUE(catalog.add(logic));
    ASSERT_TRUE(catalog.finalize());
    rmw::engine::LocalModuleRunner runner;
    runner.add(appearance);
    runner.add(system);
    auto state = rmw::engine::StateDb::open(m_dir.path() / "state.db",
                                            {.synchronous = rmw::storage::Synchronous::Off});
    ASSERT_TRUE(state.has_value());
    rmw::engine::RiceEngine engine(*state, m_store, catalog, runner);

    m_rice.wallpaper->monitors = {rule("all", rmw::domain::WallpaperKind::Image, "assets/a.jpg")};
    m_rice.wallpaper->background = rmw::domain::Color{0, 0, 0, 0xff};
    const auto applied = engine.apply(m_rice, package());
    ASSERT_TRUE(applied.has_value()) << applied.error().toString();
    EXPECT_NE(api.files[R"(\\?\DISPLAY#AAA#1)"], utf8(m_dir.path() / "mine.jpg"));

    std::filesystem::remove(m_dir.path() / "mine.jpg"); // the user deletes the original afterwards
    const auto restored = engine.restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_EQ(api.files[R"(\\?\DISPLAY#BBB#2)"], originalSide);
    EXPECT_EQ(api.background, 0x123456u);
    // The original image comes back from the store copy taken before the first change.
    const std::filesystem::path back = api.files[R"(\\?\DISPLAY#AAA#1)"];
    EXPECT_EQ(back.parent_path(), m_dir.path() / "store");
    std::ifstream in(back, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "MINE");
}

} // namespace
