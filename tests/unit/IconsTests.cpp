#include "CursorSamples.hpp"
#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/domain/CursorFiles.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/engine/ContentStore.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/modules/Icons.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace rmw::domain;
using rmw::engine::Json;
using rmw::modules::IconsLogic;
using rmw::modules::IconsSystem;
using rmw::test::FakeIcons;
using rmw::test::FakeRegistry;

constexpr const char* kThisPc =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\DefaultIcon)";
constexpr const char* kBin =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{645FF040-5081-101B-9F08-00AA002F954E}\DefaultIcon)";

void writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

TEST(IconFiles, AcceptsIconsRefusesTheRest)
{
    EXPECT_TRUE(validateIconFile(rmw::test::cursorFile(1)).has_value());
    EXPECT_FALSE(validateIconFile(rmw::test::cursorFile(2)).has_value()); // a cursor is no icon
    auto truncated = rmw::test::cursorFile(1);
    truncated.resize(truncated.size() - 30);
    EXPECT_FALSE(validateIconFile(truncated).has_value());
    EXPECT_FALSE(validateIconFile({}).has_value());
}

TEST(IconsRice, ReadsWritesAndRefuses)
{
    Diagnostics diagnostics;
    const auto rice = parseRice(R"toml(
schema = 1
[meta]
name = "Pack"

[icons]
this-pc = "assets/icons/pc.ico"
recycle-bin = "assets/icons/bin.ico"

[icons.folders]
documents = "assets/icons/docs.ico"

[icons.apps]
"Firefox.exe" = "assets/icons/fox.ico"

[icons.types]
txt = "assets/icons/txt.ico"
".MD" = "assets/icons/md.ico"
)toml",
                                diagnostics);
    ASSERT_TRUE(rice.has_value()) << diagnostics.errorSummary("rice.toml");
    ASSERT_TRUE(rice->icons.has_value());
    EXPECT_EQ(rice->icons->desktop.size(), 2u);
    EXPECT_EQ(rice->icons->folders.size(), 1u);
    ASSERT_EQ(rice->icons->apps.size(), 1u);
    EXPECT_EQ(rice->icons->apps[0].first, "firefox.exe"); // compared in lower case
    ASSERT_EQ(rice->icons->types.size(), 2u);
    EXPECT_NE(std::ranges::find(rice->icons->types, std::string("md"), [](const auto& t) { return t.first; }),
              rice->icons->types.end()); // no dot, lower case
    const auto again = parseRice(writeRice(*rice), diagnostics);
    ASSERT_TRUE(again.has_value()) << writeRice(*rice);
    EXPECT_EQ(again->icons->apps.size(), 1u);
    EXPECT_EQ(again->icons->desktop.size(), 2u);
    EXPECT_EQ(again->icons->types, rice->icons->types);

    for (const char* bad : {
             "[icons]\nthis-pc = \"assets/pc.png\"\n",               // not an .ico
             "[icons.folders]\nattic = \"assets/a.ico\"\n",          // no such folder (warning: unknown key)
             "[icons.apps]\n\"firefox\" = \"assets/a.ico\"\n",       // not a program's name
             "[icons.apps]\n\"../x.exe\" = \"assets/a.ico\"\n",
             "[icons.types]\n\"t x t\" = \"assets/a.ico\"\n",
         })
    {
        Diagnostics d;
        const auto parsed = parseRice(std::string("schema = 1\n[meta]\nname = \"x\"\n") + bad, d);
        EXPECT_TRUE(!parsed.has_value() || !d.all().empty()) << bad;
    }
    EXPECT_TRUE(isValidProgramName("Code - Insiders.exe"));
    EXPECT_FALSE(isValidProgramName(".exe"));
    EXPECT_FALSE(isValidProgramName("a\\b.exe"));
}

class IconsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        writeBytes(package() / "assets" / "icons" / "pc.ico", rmw::test::cursorFile(1));
        m_rice.meta.name = "Icons";
    }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    rmw::Result<Json> desired()
    {
        rmw::engine::StoreAssetSink sink(m_store, package());
        return IconsLogic().desiredFromRice(m_rice, sink);
    }

    rmw::test::TempDir m_dir;
    rmw::engine::ContentStore m_store{m_dir.path() / "store"};
    Rice m_rice;
};

TEST_F(IconsTest, IconsAreValidatedAndStored)
{
    IconsConfig icons;
    icons.desktop = {{DesktopIcon::ThisPc, "assets/icons/pc.ico"}, {DesktopIcon::RecycleBin, "assets/icons/pc.ico"}};
    icons.folders = {{UserFolder::Music, "assets/icons/pc.ico"}};
    icons.apps = {{"firefox.exe", "assets/icons/pc.ico"}};
    m_rice.icons = icons;
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    const std::string location = (*fields)["this-pc"]["data"].get<std::string>();
    EXPECT_TRUE(location.ends_with(".ico,0")) << location;
    EXPECT_EQ((*fields)["recycle-bin-now"], (*fields)["recycle-bin"]);
    EXPECT_EQ((*fields)["folder:music"], location);
    EXPECT_EQ((*fields)["apps"]["byProgram"]["firefox.exe"], location);
    EXPECT_FALSE(fields->contains("network"));

    writeBytes(package() / "assets" / "icons" / "bad.ico", rmw::test::cursorFile(2));
    m_rice.icons->desktop = {{DesktopIcon::Network, "assets/icons/bad.ico"}};
    EXPECT_FALSE(desired().has_value());
}

TEST_F(IconsTest, RestorePutsTheOriginalBackOrItsCopy)
{
    const auto own = m_dir.path() / "apps" / "game.ico";
    writeBytes(own, rmw::test::cursorFile(1));
    const std::string location = own.string() + ",0";
    const std::string link = R"(C:\Desktop\Game.lnk)";
    Json state = {{"this-pc", {{"type", "string"}, {"data", location}}},
                  {"folder:music", location},
                  {"apps", {{"byShortcut", {{link, {{"program", "game.exe"}, {"icon", location}}}}}}}};
    rmw::engine::StoreAssetSink sink(m_store, package());
    ASSERT_TRUE(IconsLogic().internalize(state, sink).has_value());
    const std::string copy = state["this-pc"]["copy"].get<std::string>();
    EXPECT_NE(copy, location);
    EXPECT_TRUE(copy.ends_with(".ico,0")) << copy;
    EXPECT_EQ(state["this-pc"]["data"], location); // the original stays what Windows had
    EXPECT_EQ(state["folder:music"]["copy"], copy);
    EXPECT_EQ(state["apps"]["byShortcut"][link]["copy"], copy);
    EXPECT_TRUE(IconsLogic().fieldEquivalent("folder:music", state["folder:music"], location));
    EXPECT_TRUE(
        IconsLogic().fieldEquivalent("this-pc", state["this-pc"], Json{{"type", "string"}, {"data", location}}));

    FakeRegistry registry;
    FakeIcons icons;
    icons.links = {{link, "game.exe", R"(C:\store\other.ico,0)"}};
    IconsSystem system(registry, icons);
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(registry.values.at({kThisPc, ""}).text, location); // there: the original
    EXPECT_EQ(icons.folders.at("music"), location);
    EXPECT_EQ(icons.links[0].icon, location);

    icons.gone.insert(location); // the game was uninstalled meanwhile
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(registry.values.at({kThisPc, ""}).text, copy);
    EXPECT_EQ(icons.folders.at("music"), copy);
    EXPECT_EQ(icons.links[0].icon, copy);
}

TEST(IconsSystem, DesktopFoldersAndShortcuts)
{
    FakeRegistry registry;
    registry.values[{kThisPc, ""}] = rmw::ports::RegistryValue::fromExpandString(R"(%SystemRoot%\x.dll,-109)");
    FakeIcons icons;
    icons.folders["documents"] = R"(C:\old\docs.ico,0)";
    icons.links = {{R"(C:\Start\Firefox.lnk)", "firefox.exe", ""},
                   {R"(C:\Desktop\Firefox.lnk)", "firefox.exe", R"(C:\old\fox.ico,0)"},
                   {R"(C:\Start\Notes.lnk)", "notepad.exe", ""}};
    IconsSystem system(registry, icons);

    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->state["this-pc"]["data"], R"(%SystemRoot%\x.dll,-109)");
    EXPECT_EQ(before->state["network"], nullptr);
    EXPECT_EQ(before->state["folder:documents"], R"(C:\old\docs.ico,0)");
    EXPECT_EQ(before->state["folder:music"], nullptr);
    EXPECT_EQ(before->state["apps"]["byShortcut"].size(), 3u);

    // A rice: Firefox's shortcuts (both), This PC, the Music folder.
    const Json rice = {{"this-pc", {{"type", "expand"}, {"data", R"(C:\store\pc.ico,0)"}}},
                       {"folder:music", R"(C:\store\m.ico,0)"},
                       {"apps", {{"byProgram", {{"firefox.exe", R"(C:\store\fox.ico,0)"}}}}}};
    ASSERT_TRUE(system.apply(rice));
    EXPECT_EQ(registry.values.at({kThisPc, ""}).text, R"(C:\store\pc.ico,0)");
    EXPECT_EQ(icons.folders.at("music"), R"(C:\store\m.ico,0)");
    EXPECT_EQ(icons.links[0].icon, R"(C:\store\fox.ico,0)");
    EXPECT_EQ(icons.links[1].icon, R"(C:\store\fox.ico,0)");
    EXPECT_EQ(icons.links[2].icon, ""); // not Firefox: left alone
    EXPECT_EQ(icons.refreshes, 1);

    // What was captured before puts everything back.
    const auto after = system.capture();
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(IconsLogic().fieldEquivalent("apps", rice["apps"], after->state["apps"]));
    EXPECT_FALSE(IconsLogic().fieldEquivalent("apps", rice["apps"], before->state["apps"]));
    ASSERT_TRUE(system.apply(before->state));
    EXPECT_EQ(registry.values.at({kThisPc, ""}).text, R"(%SystemRoot%\x.dll,-109)");
    EXPECT_FALSE(icons.folders.contains("music"));
    EXPECT_EQ(icons.links[0].icon, "");
    EXPECT_EQ(icons.links[1].icon, R"(C:\old\fox.ico,0)");
    EXPECT_FALSE(registry.values.contains({kBin, "empty"}));

    // The same again changes nothing: no shortcut is written.
    const int writes = icons.shortcutWrites;
    ASSERT_TRUE(system.apply(before->state));
    EXPECT_EQ(icons.shortcutWrites, writes);
    EXPECT_FALSE(system.apply({{"apps", 3}}).has_value());
}

TEST(IconsSystem, FileTypesAndNoEmptyKeysLeft)
{
    FakeRegistry registry;
    FakeIcons icons;
    icons.progIds["txt"] = "txtfile";
    IconsSystem system(registry, icons);
    const std::vector<std::string> fields = {"type:txt", "type:xyz", "this-pc"};
    const auto before = system.captureFields(fields);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->state["type:txt"]["progId"], "txtfile");
    EXPECT_EQ(before->state["type:txt"]["icon"], nullptr);
    EXPECT_EQ(before->state["type:xyz"]["progId"], ""); // no program opens it

    const Json rice = {{"this-pc", {{"type", "expand"}, {"data", R"(C:\store\pc.ico,0)"}}},
                       {"type:txt", {{"icon", {{"type", "expand"}, {"data", R"(C:\store\txt.ico,0)"}}}}},
                       {"type:xyz", {{"icon", {{"type", "expand"}, {"data", R"(C:\store\x.ico,0)"}}}}}};
    ASSERT_TRUE(system.apply(rice)); // .xyz: left alone
    const std::string txtKey = R"(Software\Classes\txtfile\DefaultIcon)";
    EXPECT_EQ(registry.values.at({txtKey, ""}).text, R"(C:\store\txt.ico,0)");
    const auto after = system.captureFields(fields);
    ASSERT_TRUE(after.has_value());
    EXPECT_TRUE(IconsLogic().fieldEquivalent("type:txt", rice["type:txt"], after->state["type:txt"]));
    EXPECT_FALSE(IconsLogic().fieldEquivalent("type:txt", rice["type:txt"], before->state["type:txt"]));

    // The user picked another program for .txt meanwhile: the restore goes to the ProgID captured.
    icons.progIds["txt"] = "otherfile";
    ASSERT_TRUE(system.apply(before->state));
    EXPECT_FALSE(registry.values.contains({txtKey, ""}));
    EXPECT_FALSE(registry.values.contains({kThisPc, ""}));
    const std::string thisPc = kThisPc;
    for (const std::string& key : {txtKey, std::string(R"(Software\Classes\txtfile)"), thisPc,
                                   thisPc.substr(0, thisPc.rfind('\\'))})
    {
        EXPECT_NE(std::ranges::find(registry.removedKeys, key), registry.removedKeys.end()) << key;
    }
}

TEST(IconsExport, OnlyTheUsersIcoFiles)
{
    rmw::modules::CapturedLook look;
    look.icons = Json{{"this-pc", {{"type", "expand"}, {"data", R"(C:\Users\me\pc.ico,0)"}}},
                      {"network", {{"type", "expand"}, {"data", R"(%SystemRoot%\system32\imageres.dll,-25)"}}},
                      {"folder:documents", R"(C:\Users\me\docs.ico,0)"},
                      {"apps", {{"byShortcut", {{R"(C:\a.lnk)", {{"program", "firefox.exe"}, {"icon", R"(C:\Users\me\fox.ico,0)"}}},
                                                {R"(C:\b.lnk)", {{"program", "notepad.exe"}, {"icon", ""}}}}}}}};
    const auto exported = rmw::modules::exportRice("Mine", look, [](std::string_view file) {
        return std::string(file.ends_with(".ico") ? "ico" : "dll");
    });
    ASSERT_TRUE(exported.has_value());
    EXPECT_EQ(exported->files.size(), 3u);
    Diagnostics diagnostics;
    const auto rice = parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    ASSERT_TRUE(rice->icons.has_value());
    EXPECT_EQ(rice->icons->desktop.size(), 1u);
    EXPECT_EQ(rice->icons->folders.size(), 1u);
    EXPECT_EQ(rice->icons->apps.size(), 1u);
}

} // namespace
