#include "CursorSamples.hpp"
#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/engine/RiceEngine.hpp"
#include "rmw/modules/Cursors.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>

namespace
{

using namespace rmw::modules;
using rmw::engine::Json;
using rmw::test::FakeRegistry;
using rmw::test::FakeShell;

constexpr std::string_view kCursors = R"(Control Panel\Cursors)";

void writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

class CursorsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        writeBytes(package() / "cursors" / "pointer.cur", rmw::test::cursorFile());
        writeBytes(package() / "cursors" / "busy.ani", rmw::test::aniFile());
        m_rice.meta.name = "Cursors";
        m_rice.cursors = rmw::domain::CursorConfig{"cursors", std::nullopt, {}};
    }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    rmw::Result<Json> desired()
    {
        rmw::engine::StoreAssetSink sink(m_store, package());
        return CursorsLogic().desiredFromRice(m_rice, sink);
    }

    rmw::test::TempDir m_dir;
    rmw::engine::ContentStore m_store{m_dir.path() / "store"};
    rmw::domain::Rice m_rice;
};

TEST_F(CursorsTest, MapFilesAreValidatedAndStored)
{
    m_rice.cursors->map = {{rmw::domain::CursorRole::Normal, "pointer.cur"},
                           {rmw::domain::CursorRole::Busy, "busy.ani"}};
    m_rice.cursors->size = 48;
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    ASSERT_TRUE(fields->contains("Arrow"));
    EXPECT_EQ((*fields)["Arrow"]["type"], "expand");
    const std::filesystem::path stored = (*fields)["Arrow"]["data"].get<std::string>();
    EXPECT_EQ(stored.parent_path(), m_dir.path() / "store");
    EXPECT_EQ(stored.extension(), ".cur");
    EXPECT_TRUE(std::filesystem::exists(stored));
    EXPECT_TRUE(fields->contains("Wait"));
    EXPECT_FALSE(fields->contains("Help")); // not in the rice: left alone
    EXPECT_EQ((*fields)["size"], 48);
    EXPECT_EQ((*fields)["schemeSource"], 0);
}

TEST_F(CursorsTest, InstallInfIsReadWhenThereIsNoMap)
{
    const std::string inf =
        "[DefaultInstall]\r\nAddReg=Scheme.Reg\r\n[Scheme.Reg]\r\n"
        "HKCU,\"Control Panel\\Cursors\\Schemes\",\"%N%\",,\"%10%\\%D%\\%p%,,,%10%\\%D%\\%b%\"\r\n"
        "[Strings]\r\nN=\"Neon\"\r\nD=\"Cursors\\Neon\"\r\np=\"pointer.cur\"\r\nb=\"busy.ani\"\r\n";
    std::ofstream(package() / "cursors" / "install.inf", std::ios::binary) << inf;
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    EXPECT_TRUE(fields->contains("Arrow"));
    EXPECT_TRUE(fields->contains("Wait"));
}

TEST_F(CursorsTest, MalformedCursorsAreRefusedBeforeReachingTheStore)
{
    auto broken = rmw::test::cursorFile();
    broken.resize(broken.size() - 20);
    writeBytes(package() / "cursors" / "broken.cur", broken);
    m_rice.cursors->map = {{rmw::domain::CursorRole::Normal, "broken.cur"}};
    const auto fields = desired();
    ASSERT_FALSE(fields.has_value());
    EXPECT_NE(fields.error().message().find("broken.cur"), std::string::npos) << fields.error().message();
    EXPECT_FALSE(std::filesystem::exists(m_dir.path() / "store")); // nothing was stored

    m_rice.cursors->map = {{rmw::domain::CursorRole::Normal, "missing.cur"}};
    EXPECT_FALSE(desired().has_value());
    m_rice.cursors->map.clear(); // no map and no install.inf
    EXPECT_FALSE(desired().has_value());
}

TEST(CursorsSystem, CapturesAndAppliesTheScheme)
{
    FakeRegistry registry;
    registry.values[{std::string(kCursors), "Arrow"}] =
        rmw::ports::RegistryValue::fromExpandString(R"(%SystemRoot%\cursors\aero_arrow.cur)");
    registry.values[{std::string(kCursors), ""}] = rmw::ports::RegistryValue::fromString("Windows Default");
    registry.setDword(kCursors, "Scheme Source", 2);
    FakeShell shell;
    CursorsSystem system(registry, shell);

    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    // Windows' own cursor: marked so the snapshot points at it instead of copying it.
    EXPECT_EQ(before->state["Arrow"],
              (Json{{"type", "expand"}, {"data", R"(%SystemRoot%\cursors\aero_arrow.cur)"}, {"keep", true}}));
    EXPECT_EQ(before->state["Help"], nullptr);
    EXPECT_EQ(before->state["scheme"]["data"], "Windows Default");
    EXPECT_EQ(before->state["schemeSource"], 2);
    EXPECT_EQ(before->state["size"], nullptr);

    ASSERT_TRUE(system.apply({{"Help", {{"type", "expand"}, {"data", R"(C:\store\h.cur)"}}},
                              {"scheme", {{"type", "string"}, {"data", ""}}},
                              {"size", 64}}));
    EXPECT_EQ(registry.values.at({std::string(kCursors), "Help"}),
              rmw::ports::RegistryValue::fromExpandString(R"(C:\store\h.cur)"));
    EXPECT_EQ(shell.cursorReloads, 1);
    EXPECT_EQ(shell.cursorSize, 64u);
    EXPECT_EQ(registry.dword(kCursors, "CursorBaseSize"), 64u);

    // Restoring "absent" removes the values again and puts the size back to the default.
    ASSERT_TRUE(system.apply({{"Help", nullptr}, {"size", nullptr}}));
    EXPECT_FALSE(registry.values.contains({std::string(kCursors), "Help"}));
    EXPECT_EQ(shell.cursorSize, 32u);
    EXPECT_FALSE(registry.dword(kCursors, "CursorBaseSize").has_value());

    EXPECT_FALSE(system.apply({{"Arrow", "C:\\x.cur"}}).has_value()); // not the field shape
}

// Settings' own pointer files (a color or a size picked in Accessibility) live in the user's folder:
// a restore puts back their path, not our copy; the copy only when they were deleted.
TEST_F(CursorsTest, TheUsersPointerFilesComeBackByTheirOwnPath)
{
    const auto own = m_dir.path() / "Microsoft" / "Windows" / "Cursors" / "arrow_eoa.cur";
    writeBytes(own, rmw::test::cursorFile());
    Json state = {{"Arrow", {{"type", "expand"}, {"data", own.string()}}}};
    rmw::engine::StoreAssetSink sink(m_store, package());
    ASSERT_TRUE(CursorsLogic().internalize(state, sink).has_value());
    EXPECT_EQ(state["Arrow"]["data"], own.string());
    const std::string copy = state["Arrow"]["copy"].get<std::string>();
    EXPECT_NE(copy, own.string());
    EXPECT_TRUE(CursorsLogic().fieldEquivalent("Arrow", state["Arrow"],
                                               Json{{"type", "expand"}, {"data", own.string()}}));

    FakeRegistry registry;
    FakeShell shell;
    CursorsSystem system(registry, shell);
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(registry.values.at({std::string(kCursors), "Arrow"}).text, own.string());
    shell.gone.insert(own.string());
    ASSERT_TRUE(system.apply(state));
    EXPECT_EQ(registry.values.at({std::string(kCursors), "Arrow"}).text, copy);
}

TEST_F(CursorsTest, ApplyThenRestoreOriginalUnderTheEngine)
{
    FakeRegistry registry;
    registry.values[{std::string(kCursors), "Arrow"}] =
        rmw::ports::RegistryValue::fromExpandString(R"(%SystemRoot%\cursors\aero_arrow.cur)");
    registry.values[{std::string(kCursors), ""}] = rmw::ports::RegistryValue::fromString("Windows Default");
    registry.setDword(kCursors, "Scheme Source", 2);
    const auto original = registry.values;
    FakeShell shell;

    const CursorsLogic logic;
    CursorsSystem system(registry, shell);
    rmw::engine::ModuleCatalog catalog;
    // The cursors module depends on appearance; a stand-in keeps the catalog valid.
    struct Appearance final : rmw::engine::IModuleLogic
    {
        std::string_view id() const noexcept override { return "appearance"; }
        rmw::Result<Json> desiredFromRice(const rmw::domain::Rice&, rmw::engine::IAssetSink&) const override
        {
            return Json::object();
        }
    };
    const Appearance appearance;
    ASSERT_TRUE(catalog.add(appearance));
    ASSERT_TRUE(catalog.add(logic));
    ASSERT_TRUE(catalog.finalize());
    struct NoSystem final : rmw::engine::IModuleSystem
    {
        std::string_view id() const noexcept override { return "appearance"; }
        rmw::Result<rmw::engine::CaptureResult> capture() override { return rmw::engine::CaptureResult{}; }
        rmw::Result<void> apply(const Json&) override { return {}; }
    } noSystem;
    rmw::engine::LocalModuleRunner runner;
    runner.add(noSystem);
    runner.add(system);
    auto state = rmw::engine::StateDb::open(m_dir.path() / "state.db",
                                            {.synchronous = rmw::storage::Synchronous::Off});
    ASSERT_TRUE(state.has_value());
    rmw::engine::RiceEngine engine(*state, m_store, catalog, runner);

    m_rice.cursors->map = {{rmw::domain::CursorRole::Normal, "pointer.cur"}};
    ASSERT_TRUE(engine.apply(m_rice, package()).has_value());
    EXPECT_NE(registry.values.at({std::string(kCursors), "Arrow"}).text.find("store"), std::string::npos);

    const auto restored = engine.restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_EQ(registry.values, original);
}

} // namespace
