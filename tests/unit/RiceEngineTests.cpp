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

TEST_F(RiceEngineTest, FailedUndoIsFlaggedAndARestoreBringsItBack)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.failApplyAt = 1;
    m_appearance.failEveryApplyFrom = 2; // the undo of appearance fails
    build();

    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().message().find("undo also failed for appearance"), std::string::npos)
        << report.error().message();
    EXPECT_TRUE(m_engine->needsRestore().value());
    // Closed anyway: nothing waits for an undo that may never work.
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "orig-a.jpg");                // undone
    EXPECT_EQ(m_state->applied("appearance").value(), (Json{{"dark", true}})); // not undone: ours
    const auto history = m_state->history().value();
    ASSERT_EQ(history.size(), 1u);
    EXPECT_NE(history[0].label.find("not fully undone"), std::string::npos);

    m_appearance.failEveryApplyFrom = 0; // whatever blocked it is gone
    const auto restored = m_engine->restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_FALSE(m_engine->needsRestore().value());
}

// The review of v1.7: an undo that kept failing refused every later operation, restore included.
TEST_F(RiceEngineTest, AnUndoThatKeepsFailingBlocksNothing)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.crashApplyAt = 1;
    build();
    EXPECT_THROW(
        { [[maybe_unused]] const auto ignored = m_engine->apply(m_rice, package()); }, std::runtime_error);

    // Next start: appearance cannot be put back.
    m_appearance.failEveryApplyFrom = m_appearance.applyCount + 1;
    m_engine.emplace(*m_state, *m_store, *m_catalog, *m_runner);
    const auto recovered = m_engine->recover();
    ASSERT_TRUE(recovered.has_value()) << recovered.error().toString();
    EXPECT_TRUE(*recovered);
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_TRUE(m_engine->needsRestore().value());

    // Another rice still applies.
    m_appearance.desired = nullptr;
    m_wallpaper.desired = wants({{"monitor:B", "new-b.jpg"}});
    const auto applied = m_engine->apply(m_rice, package());
    ASSERT_TRUE(applied.has_value()) << applied.error().toString();
    EXPECT_EQ(m_wallpaper.fields()["monitor:B"], "new-b.jpg");

    // And the restore goes as far as it can, then all the way once appearance works again.
    const auto partly = m_engine->restoreOriginal();
    ASSERT_TRUE(partly.has_value()) << partly.error().toString();
    ASSERT_EQ(partly->failed.size(), 1u);
    EXPECT_EQ(partly->failed[0].module, "appearance");
    EXPECT_EQ(m_wallpaper.fields()["monitor:B"], "orig-b.jpg");
    EXPECT_TRUE(m_engine->needsRestore().value());
    m_appearance.failEveryApplyFrom = 0;
    ASSERT_TRUE(m_engine->restoreOriginal().has_value());
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_FALSE(m_engine->needsRestore().value());
}

TEST_F(RiceEngineTest, DiscardForgetsAnInterruptedOperationWithoutUndoingIt)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.crashApplyAt = 1;
    build();
    EXPECT_THROW(
        { [[maybe_unused]] const auto ignored = m_engine->apply(m_rice, package()); }, std::runtime_error);
    const int applies = m_appearance.applyCount + m_wallpaper.applyCount;
    const auto calls = m_world.calls.size();

    m_engine.emplace(*m_state, *m_store, *m_catalog, *m_runner);
    const auto discarded = m_engine->discardJournal();
    ASSERT_TRUE(discarded.has_value()) << discarded.error().toString();
    EXPECT_TRUE(*discarded);
    EXPECT_EQ(m_appearance.applyCount + m_wallpaper.applyCount, applies); // nothing was undone
    EXPECT_EQ(m_world.calls.size(), calls);                              // no module code ran at all
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_FALSE(m_engine->needsRestore().value());
    EXPECT_FALSE(m_engine->discardJournal().value()); // nothing left

    // What it changed counts as ours: the original comes back.
    ASSERT_TRUE(m_engine->restoreOriginal().has_value());
    EXPECT_EQ(m_appearance.fields()["dark"], false);
}

// The review of v1.7: a crash in the middle of a restore applied the rice again on the next start.
TEST_F(RiceEngineTest, AnInterruptedRestoreIsFinishedNotUndone)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_wallpaper.crashApplyAt = m_wallpaper.applyCount + 1; // appearance is back, then the crash
    EXPECT_THROW(
        { [[maybe_unused]] const auto ignored = m_engine->restoreOriginal(); }, std::runtime_error);
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_EQ(m_state->openJournal().value()->operation, "restore");

    m_engine.emplace(*m_state, *m_store, *m_catalog, *m_runner);
    const auto recovered = m_engine->recover();
    ASSERT_TRUE(recovered.has_value()) << recovered.error().toString();
    EXPECT_TRUE(*recovered);
    EXPECT_EQ(m_appearance.fields()["dark"], false); // not riced again
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "orig-a.jpg");
    EXPECT_TRUE(m_state->appliedModules().value().empty());
    EXPECT_EQ(m_state->history().value().size(), 2u);
    EXPECT_FALSE(m_state->openJournal().value().has_value());
}

TEST_F(RiceEngineTest, ARestoreGoesOnPastAModuleItCannotRead)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_wallpaper.failCapture = true;

    const auto report = m_engine->restoreOriginal();
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    ASSERT_EQ(report->failed.size(), 1u);
    EXPECT_EQ(report->failed[0].module, "wallpaper");
    EXPECT_EQ(report->modules, (std::vector<std::string>{"appearance"}));
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_FALSE(m_state->applied("wallpaper").value().empty()); // still ours

    // Nothing at all comes back: that is a failure.
    EXPECT_FALSE(m_engine->restoreOriginal().has_value());
}

// The review of v1.7: a wallpaper on a monitor that was not connected was recorded as restored.
TEST_F(RiceEngineTest, AFieldWindowsDoesNotShowStaysOursUntilItComesBack)
{
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_wallpaper.unplugged = {"monitor:A"};

    const auto report = m_engine->restoreOriginal();
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    ASSERT_EQ(report->skipped.size(), 1u);
    EXPECT_EQ(report->skipped[0].field, "monitor:A");
    // Ours still, with the original: when the monitor is back, the original goes on it.
    EXPECT_EQ(m_state->applied("wallpaper").value(), (Json{{"monitor:A", "orig-a.jpg"}}));

    m_wallpaper.unplugged.clear();
    const std::vector<std::string> onlyWallpaper{"wallpaper"};
    ASSERT_TRUE(m_engine->reapplyManaged(onlyWallpaper).has_value());
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "orig-a.jpg");
    ASSERT_TRUE(m_engine->restoreOriginal().has_value());
    EXPECT_TRUE(m_state->applied("wallpaper").value().empty());
}

// The review of v1.7: going back to before "Restored the original Windows" did nothing.
TEST_F(RiceEngineTest, GoingBackPastARestoreBringsTheRiceBack)
{
    m_appearance.desired = wants({{"dark", true}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_engine->setCoreState([] { return Json{{"bars", "the rice's"}}; });
    const auto restored = m_engine->restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    m_engine->setCoreState([] { return Json{{"bars", "none"}}; });

    const auto back = m_engine->restoreHistory(restored->historyId);
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_EQ(m_state->applied("appearance").value(), (Json{{"dark", true}})); // ours again
    EXPECT_EQ(back->core, (Json{{"bars", "the rice's"}}));                    // as it was then

    // A field the user changed since that restore stays theirs.
    const auto second = m_engine->restoreOriginal();
    ASSERT_TRUE(second.has_value()) << second.error().toString();
    m_appearance.fields()["dark"] = "by hand";
    const auto kept = m_engine->restoreHistory(second->historyId);
    ASSERT_TRUE(kept.has_value()) << kept.error().toString();
    ASSERT_EQ(kept->skipped.size(), 1u);
    EXPECT_EQ(kept->skipped[0].reason, "changed after it was applied");
    EXPECT_EQ(m_appearance.fields()["dark"], "by hand");
}

TEST_F(RiceEngineTest, TheOriginalComesBackFromItsCopyWhenStateDbLosesIt)
{
    m_appearance.desired = wants({{"dark", true}});
    build();
    const auto backup = m_dir.path() / "original.json";
    m_engine->setOriginalBackup(backup);
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    ASSERT_TRUE(std::filesystem::exists(backup));

    // A new, empty state.db (the old one was damaged): Windows is riced now.
    auto fresh = StateDb::open(m_dir.path() / "fresh.db", {.synchronous = rmw::storage::Synchronous::Off});
    ASSERT_TRUE(fresh.has_value()) << fresh.error().toString();
    RiceEngine engine(*fresh, *m_store, *m_catalog, *m_runner);
    engine.setOriginalBackup(backup);
    m_appearance.desired = wants({{"accent", "#ff0000"}});
    ASSERT_TRUE(engine.apply(m_rice, package()).has_value());
    const auto base = fresh->baseSnapshot().value();
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->modules["appearance"]["state"]["dark"], false); // the real original, not the rice

    ASSERT_TRUE(engine.restoreOriginal({.force = true}).has_value());
    EXPECT_EQ(m_appearance.fields()["accent"], "#000000");
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
    // A rice of Ricing Manager's bars only (Windows' taskbar kept): the core shows them; the engine
    // has nothing to do and says so without failing.
    const auto barsOnly = m_engine->apply(m_rice, package(), true);
    ASSERT_TRUE(barsOnly.has_value()) << barsOnly.error().toString();
    EXPECT_EQ(barsOnly->historyId, 0);
    EXPECT_TRUE(barsOnly->modules.empty());
    EXPECT_TRUE(applyCalls().empty());
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

// The owner's PC (v1.6): the wallpaper of the original was gone, and the whole restore was undone.
TEST_F(RiceEngineTest, ARestoreGoesOnPastAModuleThatFails)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_wallpaper.failEveryApplyFrom = m_wallpaper.applyCount + 1;

    const auto report = m_engine->restoreOriginal();
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_EQ(report->modules, (std::vector<std::string>{"appearance"}));
    ASSERT_EQ(report->failed.size(), 1u);
    EXPECT_EQ(report->failed[0].module, "wallpaper");
    EXPECT_NE(report->failed[0].reason.find("injected"), std::string::npos);
    EXPECT_EQ(m_appearance.fields()["dark"], false);              // back
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "new-a.jpg");     // stays as it was
    EXPECT_TRUE(m_state->applied("appearance").value().empty());  // no longer ours
    EXPECT_FALSE(m_state->applied("wallpaper").value().empty());  // still ours: tried again next time
    EXPECT_EQ(m_state->history().value().size(), 2u);
    EXPECT_FALSE(m_state->openJournal().value().has_value());

    // Whatever blocked it is gone: the next restore brings the rest back.
    m_wallpaper.failEveryApplyFrom = 0;
    const auto again = m_engine->restoreOriginal();
    ASSERT_TRUE(again.has_value()) << again.error().toString();
    EXPECT_EQ(again->modules, (std::vector<std::string>{"wallpaper"}));
    EXPECT_TRUE(again->failed.empty());
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "orig-a.jpg");
}

TEST_F(RiceEngineTest, ARestoreWhereNothingComesBackFailsAndRecordsNothing)
{
    m_appearance.desired = wants({{"dark", true}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    m_appearance.failEveryApplyFrom = m_appearance.applyCount + 1;

    const auto report = m_engine->restoreOriginal();
    ASSERT_FALSE(report.has_value());
    EXPECT_NE(report.error().message().find("injected"), std::string::npos);
    EXPECT_EQ(m_state->history().value().size(), 1u);
    EXPECT_FALSE(m_state->openJournal().value().has_value());
    EXPECT_FALSE(m_engine->needsRestore().value());
}

TEST_F(RiceEngineTest, AnApplyStaysAllOrNothing)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "new-a.jpg"}});
    m_wallpaper.failEveryApplyFrom = 1;
    build();
    const auto report = m_engine->apply(m_rice, package());
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(m_appearance.fields()["dark"], false); // undone
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
    std::ofstream(package() / "assets" / "a.jpg", std::ios::binary) << "\xFF\xD8\xFF" << "AAA"; // a JPEG's start
    std::ofstream(package() / "assets" / "b.jpg", std::ios::binary) << "\xFF\xD8\xFF" << "BBB";
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

TEST_F(RiceEngineTest, AutomaticChangesLeaveNoHistory)
{
    m_appearance.desired = wants({{"dark", true}});
    build();
    const auto report = m_engine->applyAutomatic(m_rice, package(), "Schedule: dark");
    ASSERT_TRUE(report.has_value()) << report.error().toString();
    EXPECT_EQ(report->historyId, 0);
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_TRUE(m_state->history().value().empty());
    EXPECT_EQ(m_state->applied("appearance").value(), (Json{{"dark", true}}));
    // The original is still known, so a restore works after automatic changes only.
    ASSERT_TRUE(m_engine->restoreOriginal().has_value());
    EXPECT_EQ(m_appearance.fields()["dark"], false);
}

TEST_F(RiceEngineTest, DriftIsDetectedAndDetachedFieldsAreLeftAlone)
{
    m_appearance.desired = wants({{"dark", true}, {"accent", "#ff0000"}});
    m_wallpaper.desired = wants({{"monitor:A", "rice-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    EXPECT_TRUE(m_engine->driftedFields("appearance").value().empty());

    m_wallpaper.fields()["monitor:A"] = "chosen-in-settings.jpg"; // the user, through Windows Settings
    const auto drifted = m_engine->driftedFields("wallpaper");
    ASSERT_TRUE(drifted.has_value());
    EXPECT_EQ(*drifted, (std::vector<std::string>{"monitor:A"}));
    ASSERT_TRUE(m_engine->detach("wallpaper", *drifted));
    EXPECT_TRUE(m_state->applied("wallpaper").value().empty());

    // Explorer restarts: only what is still ours comes back; the user's wallpaper stays.
    m_appearance.fields()["dark"] = false; // Windows lost it in the restart
    const auto reapplied = m_engine->reapplyManaged();
    ASSERT_TRUE(reapplied.has_value()) << reapplied.error().toString();
    EXPECT_EQ(reapplied->historyId, 0);
    EXPECT_EQ(m_appearance.fields()["dark"], true);
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "chosen-in-settings.jpg");
    EXPECT_EQ(m_state->history().value().size(), 1u);

    EXPECT_FALSE(m_engine->driftedFields("nope").has_value());
}

TEST_F(RiceEngineTest, AutomaticWorkLeavesWhatIsAlreadySoAlone)
{
    m_appearance.desired = wants({{"dark", true}});
    m_wallpaper.desired = wants({{"monitor:A", "rice-a.jpg"}});
    build();
    ASSERT_TRUE(m_engine->applyAutomatic(m_rice, package(), "Schedule: dark").has_value());
    const int appearanceApplies = m_appearance.applyCount;
    const int wallpaperApplies = m_wallpaper.applyCount;

    // After resume the schedule checks again: Windows is already dark, nothing is touched.
    const auto again = m_engine->applyAutomatic(m_rice, package(), "Schedule: dark");
    ASSERT_TRUE(again.has_value()) << again.error().toString();
    EXPECT_TRUE(again->modules.empty());
    EXPECT_EQ(m_appearance.applyCount, appearanceApplies);
    EXPECT_EQ(m_wallpaper.applyCount, wallpaperApplies);
    EXPECT_FALSE(m_state->openJournal().value().has_value());

    // A monitor came back with another wallpaper: only the wallpaper module is reapplied.
    m_wallpaper.fields()["monitor:A"] = "windows-default.jpg";
    m_appearance.fields()["dark"] = false;
    const std::vector<std::string> onlyWallpaper{"wallpaper"};
    const auto reapplied = m_engine->reapplyManaged(onlyWallpaper);
    ASSERT_TRUE(reapplied.has_value()) << reapplied.error().toString();
    EXPECT_EQ(reapplied->modules, onlyWallpaper);
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], m_state->applied("wallpaper").value()["monitor:A"]);
    EXPECT_EQ(m_appearance.fields()["dark"], false);
    EXPECT_EQ(m_appearance.applyCount, appearanceApplies);
}

TEST_F(RiceEngineTest, FieldsChosenByTheCoreAreAnAutomaticChange)
{
    m_wallpaper.desired = wants({{"monitor:A", "rice-a.jpg"}, {"monitor:B", "rice-b.jpg"}});
    build();
    ASSERT_TRUE(m_engine->apply(m_rice, package()).has_value());
    const auto historyBefore = m_state->history().value().size();

    // The slideshow on monitor A turns a page; B and the history stay as they were.
    const auto turned = m_engine->applyFields("wallpaper", Json{{"monitor:A", "page-2.jpg"}}, "Slideshow");
    ASSERT_TRUE(turned.has_value()) << turned.error().toString();
    EXPECT_EQ(turned->historyId, 0);
    EXPECT_EQ(m_wallpaper.fields()["monitor:A"], "page-2.jpg");
    EXPECT_EQ(m_wallpaper.fields()["monitor:B"], "rice-b.jpg");
    EXPECT_EQ(m_state->applied("wallpaper").value()["monitor:A"], "page-2.jpg");
    EXPECT_EQ(m_state->history().value().size(), historyBefore);

    // Restoring the original still knows what monitor A had before the rice.
    ASSERT_TRUE(m_engine->restoreOriginal().has_value());
    EXPECT_NE(m_wallpaper.fields()["monitor:A"], "page-2.jpg");

    EXPECT_FALSE(m_engine->applyFields("nope", Json{{"x", 1}}, "x").has_value());
    EXPECT_FALSE(m_engine->applyFields("wallpaper", Json::object(), "x").has_value());
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
