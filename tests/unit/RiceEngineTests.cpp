#include "FakeModule.hpp"
#include "TempDir.hpp"

#include "rmw/engine/RiceEngine.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace
{

using namespace rmw::engine;
using rmw::test::FakeModule;
using rmw::test::FakeWorld;

class RiceEngineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::create_directories(package() / "assets");
        auto state =
            StateDb::open(m_dir.path() / "state.db", {.synchronous = rmw::storage::Synchronous::Off});
        ASSERT_TRUE(state.has_value()) << state.error().toString();
        m_state.emplace(std::move(*state));
        m_store.emplace(m_dir.path() / "store");
        m_rice.meta.name = "Test";
    }

    // Registers the modules (wallpaper first, to prove the catalog reorders by dependency).
    void build()
    {
        m_catalog.emplace();
        m_runner.emplace();
        for (FakeModule* module : {&m_wallpaper, &m_appearance})
        {
            ASSERT_TRUE(m_catalog->add(*module));
            m_runner->add(*module);
        }
        ASSERT_TRUE(m_catalog->finalize());
        m_engine.emplace(*m_state, *m_store, *m_catalog, *m_runner);
    }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    static FakeModule::DesiredFn wants(const Json& fields)
    {
        return [fields](const rmw::domain::Rice&, IAssetSink&) -> rmw::Result<Json> { return fields; };
    }

    std::vector<std::string> applyCalls() const
    {
        std::vector<std::string> calls;
        for (const auto& call : m_world.calls)
        {
            if (call.starts_with("apply:"))
            {
                calls.push_back(call);
            }
        }
        return calls;
    }

    std::size_t storeFiles() const
    {
        std::error_code ec;
        if (!std::filesystem::exists(m_dir.path() / "store", ec))
        {
            return 0;
        }
        return static_cast<std::size_t>(
            std::distance(std::filesystem::directory_iterator(m_dir.path() / "store"), {}));
    }

    rmw::test::TempDir m_dir;
    FakeWorld m_world;
    FakeModule m_appearance{"appearance", m_world, {{"dark", false}, {"accent", "#000000"}}};
    FakeModule m_wallpaper{
        "wallpaper", m_world, {{"monitor:A", "orig-a.jpg"}, {"monitor:B", "orig-b.jpg"}}, {"appearance"}};
    std::optional<StateDb> m_state;
    std::optional<ContentStore> m_store;
    std::optional<ModuleCatalog> m_catalog;
    std::optional<LocalModuleRunner> m_runner;
    std::optional<RiceEngine> m_engine;
    rmw::domain::Rice m_rice;
};

TEST_F(RiceEngineTest, AppliesInDependencyOrderAndRecordsEverything)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    build();

    const auto report = m_engine->apply(m_rice, package());
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_EQ(report->modules, (std::vector<std::string>{"appearance", "wallpaper"}));
    EXPECT_EQ(applyCalls(), (std::vector<std::string>{"apply:appearance", "apply:wallpaper"}));
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_EQ(m_wallpaper.fields()["monitor:B"], "orig-b.jpg"); // not asked for: untouched

    EXPECT_TRUE(m_state->baseSnapshot().value().has_value());
    EXPECT_EQ(m_state->history().value().size(), 1u);
    EXPECT_EQ(m_state->applied("appearance").value(), (Json{{"dark", true}}));
    EXPECT_FALSE(m_state->openJournal().value().has_value());
}

TEST_F(RiceEngineTest, FailureUndoesEveryTouchedModuleInReverseOrder)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.failApplyAt = 1; // half-applies monitor:A, then fails
    build();

    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().message().find("apply wallpaper"), std::string::npos)
        << report.error().message();
    EXPECT_EQ(applyCalls(), (std::vector<std::string>{"apply:appearance", "apply:wallpaper",
                                                      "apply:wallpaper", "apply:appearance"}));
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "orig-a.jpg"); // the failed module is undone too
    EXPECT_TRUE(m_state->history().value().empty());
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_FALSE(m_engine->needsRestore().value());
}

TEST_F(RiceEngineTest, CrashMidOperationIsUndoneOnTheNextStart)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.crashApplyAt = 1;
    build();

    EXPECT_THROW(
        { [[maybe_unused]] const auto ignored = m_engine->apply(m_rice, package()); }, std::runtime_error);
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_TRUE(m_state->openJournal().value().has_value());

    // "Next start": a fresh engine over the same state.db.
    m_engine.emplace(*m_state, *m_store, *m_catalog, *m_runner);
    const auto recovered = m_engine->recover();
    ASSERT_TRUE(recovered.has_value()) << recovered.error().toString();
    EXPECT_TRUE(*recovered);
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_FALSE(m_engine->recover().value()); // nothing left
}

TEST_F(RiceEngineTest, FailedUndoIsFlaggedAndRetried)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.failApplyAt = 1;
    m_appearance.failEveryApplyFrom = 2; // the undo of appearance fails
    build();

    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().message().find("undo also failed"), std::string::npos)
        << report.error().message();
    EXPECT_TRUE(m_engine->needsRestore().value());
    EXPECT_TRUE(m_state->openJournal().value().has_value());

    m_appearance.failEveryApplyFrom = 0; // whatever blocked it is gone
    ASSERT_TRUE(m_engine->recover().value_or(false));
    EXPECT_FALSE(m_engine->needsRestore().value());
    EXPECT_EQ(m_appearance.fields()["dark"], false);
}

TEST_F(RiceEngineTest, CaptureFailureStopsBeforeAnythingChanges)
{
    m_appearance.desired = wants({{"dark", true}});
    m_appearance.failCapture = true;
    build();
    EXPECT_FALSE(m_engine->apply(m_rice, package()).has_value());
    EXPECT_TRUE(applyCalls().empty());
}

TEST_F(RiceEngineTest, RiceThatChangesNothingIsRejected)
{
    build();
    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(report.error().code(), rmw::ErrorCode::InvalidArgument);
}

TEST_F(RiceEngineTest, RestoreOriginalPutsBackOnlyWhatWeChanged)
{
    m_appearance.desired = wants({{"dark", true}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_appearance.fields()["accent"] = "#123456"; // never managed by us: must survive

    const auto report = m_engine->restoreOriginal();
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_TRUE(report->skipped.empty());
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_EQ(m_appearance.fields()["accent"], "#123456");
    EXPECT_TRUE(m_state->applied("appearance").value().empty());
    EXPECT_EQ(m_state->history().value().size(), 2u);

    // Nothing of ours is left: restoring again is a no-op.
    const auto again = m_engine->restoreOriginal();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->historyId, 0);
}

TEST_F(RiceEngineTest, ThreeWayRestoreSkipsFieldsTheUserChangedUnlessForced)
{
    m_appearance.desired = wants({{"dark", true}, {"accent", "#ff0000"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_appearance.fields()["accent"] = "#00ff00"; // the user, through Windows Settings

    const auto report = m_engine->restoreOriginal();
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    ASSERT_EQ(report->skipped.size(), 1u);
    EXPECT_EQ(report->skipped[0].field, "accent");
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_EQ(m_appearance.fields()["accent"], "#00ff00");

    ASSERT_TRUE(m_engine->restoreOriginal({.force = true}).has_value());
    EXPECT_EQ(m_appearance.fields()["accent"], "#000000");
}

TEST_F(RiceEngineTest, RestoreHistoryGoesBackOneStep)
{
    build();
    m_appearance.desired = wants({{"dark", true}});
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_appearance.desired = wants({{"dark", false}, {"accent", "#111111"}});
    const auto second = m_engine->apply(m_rice, package());
    ASSERT_TRUE(second.has_value());

    const auto report = m_engine->restoreHistory(second->historyId);
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_EQ(m_appearance.fields()["accent"], "#000000");
    EXPECT_FALSE(m_engine->restoreHistory(9999).has_value());
}

TEST_F(RiceEngineTest, HistoryKeepsTheNewestEntries)
{
    build();
    for (int i = 0; i < 25; ++i)
    {
        m_appearance.desired = wants({{"dark", i % 2 == 0}});
        ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    }
    const auto history = m_state->history().value();
    ASSERT_EQ(history.size(), RiceEngine::kHistoryLimit);
    EXPECT_GT(history.front().id, history.back().id);
}

TEST_F(RiceEngineTest, LossySnapshotsAreReported)
{
    m_appearance.desired = wants({{"dark", true}});
    m_appearance.lossy = true;
    build();
    const auto report = m_engine->apply(m_rice, package());
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->warnings.size(), 1u);
    EXPECT_NE(report->warnings[0].find("lossy"), std::string::npos);
}

TEST_F(RiceEngineTest, PackageAssetsGoToTheStoreAndOrphansAreCollected)
{
    std::ofstream(package() / "assets" / "a.jpg", std::ios::binary) << "AAA";
    std::ofstream(package() / "assets" / "b.jpg", std::ios::binary) << "BBB";
    std::string assetName = "assets/b.jpg";
    m_wallpaper.desired = [&](const rmw::domain::Rice&, IAssetSink& assets) -> rmw::Result<Json> {
        auto stored = assets.fromPackage(assetName);
        if (!stored)
        {
            return std::unexpected(stored.error());
        }
        return Json{{"monitor:A", stored->string()}};
    };
    m_wallpaper.failApplyAt = 1;
    build();

    // The failed apply stored b.jpg, which nothing references afterwards.
    ASSERT_FALSE(m_engine->apply(m_rice, package()).has_value());
    EXPECT_EQ(storeFiles(), 1u);

    assetName = "assets/a.jpg";
    const auto report = m_engine->apply(m_rice, package());
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_TRUE(report->warnings.empty());
    EXPECT_EQ(storeFiles(), 1u); // b.jpg collected, a.jpg kept
    const std::string stored = m_wallpaper.fields()["monitor:A"];
    EXPECT_TRUE(std::filesystem::exists(stored));
    EXPECT_EQ(std::filesystem::path(stored).parent_path(), m_dir.path() / "store");
    EXPECT_EQ(std::filesystem::path(stored).extension(), ".jpg");
}

TEST_F(RiceEngineTest, PackageLinksCannotEscapeTheRiceFolder)
{
    std::filesystem::create_directories(m_dir.path() / "outside");
    std::ofstream(m_dir.path() / "outside" / "secret.jpg", std::ios::binary) << "S";
    const std::string command =
        std::format(R"(mklink /J "{}" "{}" >nul 2>nul)", (package() / "link").string(),
                    (m_dir.path() / "outside").string());
    if (std::system(command.c_str()) != 0)
    {
        GTEST_SKIP() << "cannot create a junction here";
    }
    m_wallpaper.desired = [](const rmw::domain::Rice&, IAssetSink& assets) -> rmw::Result<Json> {
        auto stored = assets.fromPackage("link/secret.jpg");
        if (!stored)
        {
            return std::unexpected(stored.error());
        }
        return Json{{"monitor:A", stored->string()}};
    };
    build();
    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(report.error().code(), rmw::ErrorCode::AccessDenied) << report.error().toString();
}

TEST(ModuleCatalog, OrdersByDependencyAndRejectsBadGraphs)
{
    FakeWorld world;
    const FakeModule a("a", world, {}, {"b"});
    const FakeModule b("b", world);
    ModuleCatalog catalog;
    ASSERT_TRUE(catalog.add(a));
    ASSERT_TRUE(catalog.add(b));
    EXPECT_EQ(catalog.add(b).error().code(), rmw::ErrorCode::AlreadyExists);
    ASSERT_TRUE(catalog.finalize());
    ASSERT_EQ(catalog.ordered().size(), 2u);
    EXPECT_EQ(catalog.ordered()[0]->id(), "b");

    const FakeModule c("c", world, {}, {"d"});
    const FakeModule d("d", world, {}, {"c"});
    ModuleCatalog cyclic;
    ASSERT_TRUE(cyclic.add(c));
    ASSERT_TRUE(cyclic.add(d));
    EXPECT_FALSE(cyclic.finalize().has_value());

    const FakeModule e("e", world, {}, {"missing"});
    ModuleCatalog dangling;
    ASSERT_TRUE(dangling.add(e));
    EXPECT_FALSE(dangling.finalize().has_value());

    const FakeModule bad("Bad Id", world);
    EXPECT_FALSE(ModuleCatalog().add(bad).has_value());
}

} // namespace
