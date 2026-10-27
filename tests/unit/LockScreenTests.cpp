#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/engine/ContentStore.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/modules/LockScreen.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace rmw::domain;
using rmw::engine::Json;
using rmw::modules::LockScreenLogic;
using rmw::modules::LockScreenSystem;
using rmw::test::FakeLockScreen;
using rmw::test::FakeRegistry;

constexpr const char* kDelivery = R"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)";

// The first bytes of a PNG that declares `width` x `height` (enough for its size check).
std::vector<char> pngHead(std::uint32_t width, std::uint32_t height)
{
    std::vector<char> bytes = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (const std::uint32_t side : {width, height})
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<char>((side >> shift) & 0xFFU));
        }
    }
    bytes.insert(bytes.end(), {8, 6, 0, 0, 0, 0, 0, 0, 0});
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<char>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

TEST(LockScreenRice, ReadsWritesAndRefuses)
{
    Diagnostics diagnostics;
    const auto rice = parseRice(R"toml(
schema = 1
[meta]
name = "Lock"

[lockscreen]
image = "assets/lock.png"
tips = false
)toml",
                                diagnostics);
    ASSERT_TRUE(rice.has_value()) << diagnostics.errorSummary("rice.toml");
    ASSERT_TRUE(rice->lockscreen.has_value());
    EXPECT_EQ(rice->lockscreen->image, "assets/lock.png");
    EXPECT_EQ(rice->lockscreen->tips, false);
    EXPECT_FALSE(rice->lockscreen->spotlight.has_value());
    const auto again = parseRice(writeRice(*rice), diagnostics);
    ASSERT_TRUE(again.has_value()) << writeRice(*rice);
    EXPECT_EQ(again->lockscreen->image, "assets/lock.png");
    EXPECT_EQ(again->lockscreen->tips, false);

    for (const char* bad : {
             "[lockscreen]\nimage = \"assets/lock.gif\"\n",               // not a .jpg or .png
             "[lockscreen]\nimage = \"assets/a.jpg\"\nspotlight = true\n", // both
             "[lockscreen]\nimage = \"../a.jpg\"\n",
         })
    {
        Diagnostics d;
        EXPECT_FALSE(parseRice(std::string("schema = 1\n[meta]\nname = \"x\"\n") + bad, d).has_value()) << bad;
    }
    Diagnostics d;
    EXPECT_TRUE(parseRice("schema = 1\n[meta]\nname = \"x\"\n[lockscreen]\nspotlight = true\n", d).has_value());
}

class LockScreenTest : public ::testing::Test
{
protected:
    void SetUp() override { m_rice.meta.name = "Lock"; }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    rmw::Result<Json> desired()
    {
        rmw::engine::StoreAssetSink sink(m_store, package());
        return LockScreenLogic().desiredFromRice(m_rice, sink);
    }

    rmw::test::TempDir m_dir;
    rmw::engine::ContentStore m_store{m_dir.path() / "store"};
    Rice m_rice;
};

TEST_F(LockScreenTest, PicturesAreCheckedAndStored)
{
    writeFile(package() / "assets" / "lock.png", pngHead(1920, 1080));
    m_rice.lockscreen = LockScreenConfig{"assets/lock.png", std::nullopt, true};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    EXPECT_EQ((*fields)["picture"]["spotlight"], false);
    EXPECT_TRUE((*fields)["picture"]["file"].get<std::string>().ends_with(".png"));
    EXPECT_EQ((*fields)["tips"], true);
    EXPECT_EQ((*fields)["tips-content"], true);

    // A small file that declares a huge picture, and a file that is no picture at all.
    writeFile(package() / "assets" / "bomb.png", pngHead(100'000, 100'000));
    m_rice.lockscreen->image = "assets/bomb.png";
    EXPECT_FALSE(desired().has_value());
    writeFile(package() / "assets" / "text.png", {'h', 'e', 'l', 'l', 'o'});
    m_rice.lockscreen->image = "assets/text.png";
    EXPECT_FALSE(desired().has_value());

    m_rice.lockscreen = LockScreenConfig{"", true, std::nullopt};
    EXPECT_EQ(*desired(), (Json{{"picture", {{"spotlight", true}}}}));
}

TEST_F(LockScreenTest, ApplyCaptureAndRestore)
{
    FakeRegistry registry;
    registry.values[{kDelivery, "RotatingLockScreenEnabled"}] = rmw::ports::RegistryValue::fromDword(1);
    FakeLockScreen lockScreen;
    lockScreen.source = R"(C:\Windows\Web\Screen\img100.jpg)";
    LockScreenSystem system(registry, lockScreen);

    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->state["picture"]["spotlight"], true);
    EXPECT_EQ(before->state["picture"]["file"], lockScreen.source);
    EXPECT_EQ(before->state["tips"], nullptr);
    EXPECT_EQ(before->fidelity, rmw::engine::Fidelity::Lossy); // Spotlight: its next picture

    const Json rice = {{"picture", {{"spotlight", false}, {"file", R"(C:\store\lock.png)"}}}, {"tips", false}};
    ASSERT_TRUE(system.apply(rice));
    EXPECT_EQ(lockScreen.source, R"(C:\store\lock.png)");
    EXPECT_EQ(registry.values.at({kDelivery, "RotatingLockScreenEnabled"}).dword, 0u);
    EXPECT_EQ(registry.values.at({kDelivery, "RotatingLockScreenOverlayEnabled"}).dword, 0u);
    const auto after = system.capture();
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(LockScreenLogic().fieldEquivalent("picture", rice["picture"], after->state["picture"]));
    EXPECT_FALSE(LockScreenLogic().fieldEquivalent("picture", rice["picture"], before->state["picture"]));

    // The same again sets nothing.
    const int sets = lockScreen.sets;
    ASSERT_TRUE(system.apply(rice));
    EXPECT_EQ(lockScreen.sets, sets);

    // What was captured before puts everything back: the picture (the one Windows keeps for "Picture"), then
    // Spotlight on.
    const Json restore = before->state;
    ASSERT_TRUE(system.apply(restore));
    EXPECT_EQ(lockScreen.source, R"(C:\Windows\Web\Screen\img100.jpg)");
    EXPECT_EQ(registry.values.at({kDelivery, "RotatingLockScreenEnabled"}).dword, 1u);
    EXPECT_FALSE(registry.values.contains({kDelivery, "RotatingLockScreenOverlayEnabled"}));

    EXPECT_FALSE(system.apply({{"picture", 3}}).has_value());
    EXPECT_FALSE(system.apply({{"tips", "yes"}}).has_value());
}

TEST_F(LockScreenTest, AUsersPictureGetsACopyUsedWhenItIsGone)
{
    const auto own = m_dir.path() / "Pictures" / "me.jpg";
    writeFile(own, {'\xff', '\xd8', '\xff', '\xe0'});
    Json state = {{"picture", {{"spotlight", false}, {"file", own.string()}}}};
    rmw::engine::StoreAssetSink sink(m_store, package());
    ASSERT_TRUE(LockScreenLogic().internalize(state, sink).has_value());
    const std::string copy = state["picture"]["copy"].get<std::string>();
    EXPECT_NE(copy, own.string());
    EXPECT_EQ(state["picture"]["file"], own.string());
    EXPECT_TRUE(LockScreenLogic().fieldEquivalent("picture", state["picture"],
                                                  Json{{"spotlight", false}, {"file", own.string()}}));

    FakeRegistry registry;
    FakeLockScreen lockScreen;
    LockScreenSystem system(registry, lockScreen);
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(lockScreen.source, own.string()); // there: the user's own file
    lockScreen.gone.insert(own.string());
    lockScreen.source.clear();
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(lockScreen.source, copy);

    // Both gone (a data folder deleted under a running core): the picture stays, the restore goes on.
    lockScreen.gone.insert(copy);
    lockScreen.source = "rice.jpg";
    lockScreen.sets = 0;
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(lockScreen.sets, 0);
    EXPECT_EQ(lockScreen.source, "rice.jpg");

    // Windows' own pictures are never copied.
    Json windows = {{"picture", {{"spotlight", true}, {"file", R"(C:\Windows\Web\Screen\img100.jpg)"}}}};
    ASSERT_TRUE(LockScreenLogic().internalize(windows, sink).has_value());
    EXPECT_FALSE(windows["picture"].contains("copy"));
}

TEST(LockScreenSystem, ABrokenApiStillCapturesSpotlight)
{
    FakeRegistry registry;
    FakeLockScreen lockScreen;
    lockScreen.broken = true;
    LockScreenSystem system(registry, lockScreen);
    const auto captured = system.capture();
    ASSERT_TRUE(captured.has_value());
    EXPECT_FALSE(captured->state["picture"].contains("file"));
    EXPECT_EQ(captured->state["picture"]["spotlight"], nullptr);
}

TEST(LockScreenExport, SpotlightOrTheUsersPicture)
{
    const auto extensionOf = [](std::string_view file) {
        return std::string(file.ends_with(".png") ? "png" : file.ends_with(".jpg") ? "jpg" : "");
    };
    rmw::modules::CapturedLook look;
    look.lockscreen = Json{{"picture", {{"spotlight", false}, {"file", R"(C:\Users\me\a.png)"}, {"copy", R"(C:\store\x.png)"}}},
                           {"tips", false}};
    auto exported = rmw::modules::exportRice("Mine", look, extensionOf);
    ASSERT_TRUE(exported.has_value());
    ASSERT_EQ(exported->files.size(), 1u);
    EXPECT_EQ(exported->files[0].source, R"(C:\store\x.png)");
    Diagnostics diagnostics;
    auto rice = parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    EXPECT_EQ(rice->lockscreen->image, "assets/lockscreen.png");
    EXPECT_EQ(rice->lockscreen->tips, false);

    look.lockscreen = Json{{"picture", {{"spotlight", true}, {"file", R"(C:\Windows\Web\Screen\img100.jpg)"}}}};
    exported = rmw::modules::exportRice("Mine", look, extensionOf);
    ASSERT_TRUE(exported.has_value());
    EXPECT_TRUE(exported->files.empty());
    rice = parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    EXPECT_EQ(rice->lockscreen->spotlight, true);
}

} // namespace
