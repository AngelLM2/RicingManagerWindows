#include "rmw/domain/RiceParser.hpp"
#include "rmw/settings/Model.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using namespace rmw;

class SettingsModel : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_root = fs::temp_directory_path() /
                 fs::path(std::format(L"rmw-settings-model-{}", GetCurrentProcessId())) /
                 fs::path(::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(m_root);
        fs::create_directories(m_root / L"data");
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    fs::path data() const { return m_root / L"data"; }

    fs::path write(const fs::path& relative, std::string_view bytes) const
    {
        const fs::path file = m_root / relative;
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file;
    }

    // A tiny real PNG (the importer looks at the first bytes).
    static constexpr std::string_view kPng{"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0",
                                           29};

    fs::path m_root;
};

TEST_F(SettingsModel, NewFoldersHaveTheirOwnNames)
{
    auto first = settings::newRiceFolder(data(), "My look");
    auto second = settings::newRiceFolder(data(), "My look");
    auto odd = settings::newRiceFolder(data(), "a<b>:c/d?");
    ASSERT_TRUE(first && second && odd);
    EXPECT_EQ(first->filename(), L"My look");
    EXPECT_EQ(second->filename(), L"My look 2");
    EXPECT_EQ(odd->filename(), L"a-b--c-d-");
    EXPECT_TRUE(fs::is_directory(*second));
}

TEST_F(SettingsModel, SavedRicesReadBackAndKnowWhoWroteThem)
{
    auto folder = settings::newRiceFolder(data(), "Night");
    ASSERT_TRUE(folder);
    domain::Rice rice;
    rice.meta.name = "Night";
    rice.appearance = domain::AppearanceConfig{};
    rice.appearance->mode = domain::ThemeMode::Dark;
    ASSERT_TRUE(settings::saveRice(*folder, rice));
    auto loaded = settings::loadRice(*folder);
    ASSERT_TRUE(loaded) << loaded.error().message();
    EXPECT_FALSE(loaded->handWritten);
    EXPECT_EQ(loaded->rice.appearance->mode, domain::ThemeMode::Dark);

    write(L"data/rices/Hand/rice.toml", "# my notes\nschema = 1\n[meta]\nname = \"Hand\"\n");
    auto hand = settings::loadRice(data() / L"rices" / L"Hand");
    ASSERT_TRUE(hand);
    EXPECT_TRUE(hand->handWritten);

    // `rmw export` opens its file with a note of its own: not the user's comments.
    write(L"data/rices/Mine/rice.toml",
          "# Exported by Ricing Manager 1.1.0 from this computer.\n# Monitors are matched by identity.\n"
          "schema = 1\n[meta]\nname = \"Mine\"\n");
    auto exported = settings::loadRice(data() / L"rices" / L"Mine");
    ASSERT_TRUE(exported);
    EXPECT_FALSE(exported->handWritten);
    write(
        L"data/rices/Mixed/rice.toml",
        "# Exported by Ricing Manager 1.1.0 from this computer.\nschema = 1\n# mine\n[meta]\nname = \"M\"\n");
    auto mixed = settings::loadRice(data() / L"rices" / L"Mixed");
    ASSERT_TRUE(mixed);
    EXPECT_TRUE(mixed->handWritten);
}

TEST_F(SettingsModel, ImagesComeIntoTheRiceOnce)
{
    auto folder = settings::newRiceFolder(data(), "R");
    ASSERT_TRUE(folder);
    const auto image = write(L"pictures/city night.png", kPng);
    auto first = settings::importAsset(*folder, image, settings::AssetKind::Image);
    ASSERT_TRUE(first) << first.error().message();
    EXPECT_EQ(*first, "assets/wallpapers/city night.png");
    // The same content again: the same file.
    auto again = settings::importAsset(*folder, image, settings::AssetKind::Image);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *first);
    // Another picture with that name: another name.
    const auto other = write(L"other/city night.png", std::string(kPng) + "more");
    auto renamed = settings::importAsset(*folder, other, settings::AssetKind::Image);
    ASSERT_TRUE(renamed);
    EXPECT_EQ(*renamed, "assets/wallpapers/city night 2.png");
    EXPECT_TRUE(fs::exists(settings::assetPath(*folder, *renamed)));
    // What is not an image is refused.
    const auto fake = write(L"pictures/fake.png", "not a png at all");
    EXPECT_FALSE(settings::importAsset(*folder, fake, settings::AssetKind::Image));
    const auto text = write(L"pictures/notes.txt", "hello");
    EXPECT_FALSE(settings::importAsset(*folder, text, settings::AssetKind::Image));
    EXPECT_FALSE(
        settings::importAsset(*folder, m_root / L"pictures", settings::AssetKind::Image)); // a folder
}

TEST_F(SettingsModel, LongFileNamesKeepTheirExtensionAndTheRiceReads)
{
    auto folder = settings::newRiceFolder(data(), "R");
    ASSERT_TRUE(folder);
    const auto image = write(fs::path(std::wstring(90, L'w') + L".png"), kPng);
    auto stored = settings::importAsset(*folder, image, settings::AssetKind::Image);
    ASSERT_TRUE(stored) << stored.error().message();
    EXPECT_TRUE(stored->ends_with(".png")) << *stored;
    EXPECT_TRUE(fs::exists(settings::assetPath(*folder, *stored)));

    domain::Rice rice;
    rice.meta.name = "R";
    settings::wallpaperRule(rice, *domain::MonitorSelector::parse("all")).source = *stored;
    ASSERT_TRUE(settings::saveRice(*folder, rice));
    auto loaded = settings::loadRice(*folder);
    ASSERT_TRUE(loaded) << loaded.error().message();
    EXPECT_EQ(loaded->rice.wallpaper->monitors.front().source, *stored);
}

TEST_F(SettingsModel, ARiceThatWouldNotReadBackIsNotWritten)
{
    auto folder = settings::newRiceFolder(data(), "R");
    ASSERT_TRUE(folder);
    domain::Rice good;
    good.meta.name = "Good";
    ASSERT_TRUE(settings::saveRice(*folder, good));

    domain::Rice bad = good;
    bad.meta.name = "Bad";
    settings::wallpaperRule(bad, *domain::MonitorSelector::parse("all")).source = "assets/wallpapers/no-extension";
    const auto saved = settings::saveRice(*folder, bad);
    ASSERT_FALSE(saved);
    EXPECT_NE(saved.error().message().find("would not read back"), std::string::npos);
    // The file is still the last one that read.
    auto loaded = settings::loadRice(*folder);
    ASSERT_TRUE(loaded) << loaded.error().message();
    EXPECT_EQ(loaded->rice.meta.name, "Good");
}

TEST_F(SettingsModel, CursorFilesKeepTheNamesInstallInfGivesThem)
{
    auto folder = settings::newRiceFolder(data(), "R");
    ASSERT_TRUE(folder);
    write(L"Pack/Help (1).cur", "cur");
    write(L"Pack/Normal, big.ani", "ani");
    write(L"Pack/install.inf", "[Version]");
    auto stored = settings::importAsset(*folder, m_root / L"Pack", settings::AssetKind::Cursors);
    ASSERT_TRUE(stored) << stored.error().message();
    const fs::path copied = settings::assetPath(*folder, *stored);
    EXPECT_TRUE(fs::exists(copied / L"Help (1).cur"));
    EXPECT_TRUE(fs::exists(copied / L"Normal, big.ani"));
    EXPECT_TRUE(fs::exists(copied / L"install.inf"));
}

TEST_F(SettingsModel, RiceNamesAreNamesWindowsKeeps)
{
    using P = settings::RiceNameProblem;
    EXPECT_EQ(settings::checkRiceName("  Night drive  ").value(), "Night drive");
    EXPECT_EQ(settings::checkRiceName("Catppuccin (Mocha) v2.1").value(), "Catppuccin (Mocha) v2.1");
    EXPECT_EQ(settings::checkRiceName("夜景 🎴").value(), "夜景 🎴");
    EXPECT_EQ(settings::checkRiceName("console").value(), "console"); // starts with "con", is not CON
    EXPECT_EQ(settings::checkRiceName("   ").error(), P::Empty);
    EXPECT_EQ(settings::checkRiceName(std::string(129, 'a')).error(), P::TooLong);
    for (const char* bad : {"a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\tb"})
    {
        EXPECT_EQ(settings::checkRiceName(bad).error(), P::Character) << bad;
    }
    EXPECT_EQ(settings::checkRiceName("Night.").error(), P::TrailingDot);
    for (const char* device : {"CON", "nul", "Com1", "LPT9", "aux.txt"})
    {
        EXPECT_EQ(settings::checkRiceName(device).error(), P::DeviceName) << device;
    }
}

TEST_F(SettingsModel, CursorFoldersBringOnlyCursors)
{
    auto folder = settings::newRiceFolder(data(), "R");
    ASSERT_TRUE(folder);
    write(L"Neon/normal.cur", "cur");
    write(L"Neon/busy.ani", "ani");
    write(L"Neon/install.inf", "[Version]");
    write(L"Neon/readme.txt", "no");
    auto stored = settings::importAsset(*folder, m_root / L"Neon", settings::AssetKind::Cursors);
    ASSERT_TRUE(stored) << stored.error().message();
    EXPECT_EQ(*stored, "assets/cursors/Neon");
    const fs::path copied = settings::assetPath(*folder, *stored);
    EXPECT_TRUE(fs::exists(copied / L"normal.cur"));
    EXPECT_TRUE(fs::exists(copied / L"install.inf"));
    EXPECT_FALSE(fs::exists(copied / L"readme.txt"));
    // An empty folder is refused.
    fs::create_directories(m_root / L"Empty");
    EXPECT_FALSE(settings::importAsset(*folder, m_root / L"Empty", settings::AssetKind::Cursors));
}

TEST_F(SettingsModel, TheLibraryListsRicesByName)
{
    write(L"data/rices/b/rice.toml", "schema = 1\n[meta]\nname = \"beta\"\n");
    write(L"data/rices/a/rice.toml", "schema = 1\n[meta]\nname = \"Alpha\"\n");
    write(L"data/rices/broken/rice.toml", "schema = 1\n[meta\n");
    write(L"elsewhere/rice.toml", "schema = 1\n[meta]\nname = \"Gamma\"\n");
    const std::array<fs::path, 2> extra = {m_root / L"elsewhere", data() / L"rices" / L"a"}; // "a" only once
    const auto entries = settings::scanLibrary(data(), extra);
    ASSERT_EQ(entries.size(), 4u);
    EXPECT_EQ(entries[0].name, "Alpha");
    EXPECT_EQ(entries[1].name, "beta");
    EXPECT_EQ(entries[2].name, "broken"); // the folder's name, and why
    EXPECT_FALSE(entries[2].problem.empty());
    EXPECT_EQ(entries[3].name, "Gamma");
}

TEST_F(SettingsModel, DuplicatesAreRicesOfTheirOwn)
{
    write(L"data/rices/Original/rice.toml",
          "schema = 1\n[meta]\nid = \"io.github.me.original\"\nname = \"Original\"\n[[wallpaper.monitor]]\n"
          "match = \"all\"\nsource = \"assets/a.png\"\n");
    write(L"data/rices/Original/assets/a.png", kPng);
    auto copy = settings::duplicateRice(data(), data() / L"rices" / L"Original", "Original (copy)");
    ASSERT_TRUE(copy) << copy.error().message();
    auto loaded = settings::loadRice(*copy);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->rice.meta.name, "Original (copy)");
    EXPECT_TRUE(loaded->rice.meta.id.empty());
    EXPECT_TRUE(fs::exists(*copy / L"assets" / L"a.png"));
}

TEST_F(SettingsModel, EveryMonitorStaysTheLastRule)
{
    domain::Rice rice;
    auto& all = settings::wallpaperRule(rice, *domain::MonitorSelector::parse("all"));
    all.source = "assets/a.png";
    settings::wallpaperRule(rice, *domain::MonitorSelector::parse("primary")).source = "assets/b.png";
    ASSERT_EQ(rice.wallpaper->monitors.size(), 2u);
    EXPECT_EQ(rice.wallpaper->monitors[0].match.toString(), "primary");
    EXPECT_EQ(rice.wallpaper->monitors[1].match.toString(), "all");
    // Asking again gives the same rule.
    EXPECT_EQ(settings::wallpaperRule(rice, *domain::MonitorSelector::parse("primary")).source,
              "assets/b.png");
    settings::removeWallpaperRule(rice, *domain::MonitorSelector::parse("primary"));
    EXPECT_EQ(rice.wallpaper->monitors.size(), 1u);
}

TEST_F(SettingsModel, BarMovesAndTheKeysTheyWrite)
{
    domain::BarConfig bar = settings::newBar();
    ASSERT_EQ(bar.left.size(), 3u);
    // A new bar writes every key: none is marked.
    settings::moveItem(bar, settings::Zone::Left, 0, settings::Zone::Right, 99);
    EXPECT_TRUE(bar.keys.empty());
    EXPECT_EQ(bar.left.size(), 2u);
    EXPECT_EQ(bar.right.back().id, "start");
    settings::moveItem(bar, settings::Zone::Right, bar.right.size() - 1, settings::Zone::Right, 0);
    EXPECT_EQ(bar.right.front().id, "start");
    settings::removeItem(bar, settings::Zone::Center, 0);
    EXPECT_TRUE(bar.center.empty());
    settings::addItem(bar, settings::Zone::Center, "media");
    EXPECT_EQ(bar.center.front().id, "media");

    // A bar from a skin writes only what it changes.
    domain::BarConfig skinned;
    skinned.skin = "io.github.me.skin";
    settings::addItem(skinned, settings::Zone::Left, "clock");
    EXPECT_EQ(skinned.keys, std::vector<std::string>{"left"});
    settings::markKey(skinned, "thickness");
    settings::markKey(skinned, "thickness");
    EXPECT_EQ(skinned.keys.size(), 2u);
}

TEST_F(SettingsModel, SettingsFileMissingIsEmpty)
{
    const std::array<std::string_view, 1> actions = {"appearance.toggle"};
    auto none = settings::loadSettings(data(), actions);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->hotkeys.empty());
    domain::Settings saved;
    saved.keepInTray = true;
    saved.hotkeys.push_back({*domain::KeyChord::parse("Win+Alt+D"), {"appearance.toggle", {}, {}}});
    ASSERT_TRUE(settings::saveSettings(data(), saved));
    auto loaded = settings::loadSettings(data(), actions);
    ASSERT_TRUE(loaded) << loaded.error().message();
    EXPECT_TRUE(loaded->keepInTray);
    ASSERT_EQ(loaded->hotkeys.size(), 1u);
    // A broken file is reported, not replaced.
    write(L"data/config/settings.toml", "[core\n");
    EXPECT_FALSE(settings::loadSettings(data(), actions));
}

} // namespace
