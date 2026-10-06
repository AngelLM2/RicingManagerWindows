#include "TempDir.hpp"

#include "rmw/engine/StateDb.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{

using namespace rmw::engine;
using namespace std::chrono_literals;

constexpr rmw::storage::DatabaseOptions kFast{.synchronous = rmw::storage::Synchronous::Off};

TEST(StateDb, SnapshotsKeepWhatTheOperationChangedAndTheCoreState)
{
    const rmw::test::TempDir dir;
    auto state = StateDb::open(dir.path() / "state.db", kFast);
    ASSERT_TRUE(state.has_value()) << state.error().toString();
    SnapshotRecord record;
    record.label = "Applied \"Night\"";
    record.modules = Json{{"appearance", Json{{"state", Json{{"dark", false}}}}}};
    record.changes = Json{{"appearance", Json{{"dark", true}}}};
    record.core = Json{{"barPlan", "{}"}};
    const auto id = state->addSnapshot(record);
    ASSERT_TRUE(id.has_value()) << id.error().toString();

    auto read = state->snapshot(*id).value();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->modules, record.modules); // the extras are not taken for modules
    EXPECT_EQ(read->changes, record.changes);
    EXPECT_EQ(read->core, record.core);

    // Replacing the modules keeps them.
    ASSERT_TRUE(state->setSnapshotModules(*id, Json{{"cursors", Json::object()}}).has_value());
    read = state->snapshot(*id).value();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->modules, (Json{{"cursors", Json::object()}}));
    EXPECT_EQ(read->changes, record.changes);
    EXPECT_EQ(read->core, record.core);
}

// The review of v1.7: another thread's write landed inside the engine's transaction and was lost
// with its rollback.
TEST(StateDb, AnotherThreadsWriteWaitsForATransactionToEnd)
{
    const rmw::test::TempDir dir;
    auto state = StateDb::open(dir.path() / "state.db", kFast);
    ASSERT_TRUE(state.has_value()) << state.error().toString();
    std::atomic<bool> written{false};
    std::thread other;
    {
        auto transaction = state->transaction();
        ASSERT_TRUE(transaction.has_value()) << transaction.error().toString();
        ASSERT_TRUE(state->setMeta("engine", "1").has_value());
        other = std::thread([&] {
            EXPECT_TRUE(state->setMeta("plugin", "kept").has_value());
            written = true;
        });
        std::this_thread::sleep_for(150ms);
        EXPECT_FALSE(written.load()); // waiting for the transaction
    } // rolled back
    other.join();
    EXPECT_FALSE(state->meta("engine").value().has_value());
    EXPECT_EQ(state->meta("plugin").value(), "kept");
}

TEST(StateDb, AFileThatIsNotADatabaseIsMovedAsideAndANewOneUsed)
{
    const rmw::test::TempDir dir;
    const auto file = dir.path() / "state.db";
    {
        std::ofstream garbage(file, std::ios::binary);
        for (int i = 0; i < 512; ++i)
        {
            garbage << "this is not a database, it is a damaged file ";
        }
    }
    EXPECT_FALSE(StateDb::open(file, kFast).has_value());

    std::optional<std::filesystem::path> movedTo;
    auto state = StateDb::openOrReplace(file, kFast, movedTo);
    ASSERT_TRUE(state.has_value()) << state.error().toString();
    ASSERT_TRUE(movedTo.has_value());
    EXPECT_TRUE(std::filesystem::exists(*movedTo));
    EXPECT_NE(movedTo->filename().string().find("state.db.damaged-"), std::string::npos);
    EXPECT_TRUE(state->setMeta("works", "yes").has_value());
    EXPECT_FALSE(state->baseSnapshot().value().has_value());
}

TEST(StateDb, ADamagedPageIsFoundAtOpen)
{
    const rmw::test::TempDir dir;
    const auto file = dir.path() / "state.db";
    {
        auto state = StateDb::open(file, kFast);
        ASSERT_TRUE(state.has_value()) << state.error().toString();
        for (int i = 0; i < 50; ++i)
        {
            ASSERT_TRUE(state->addSnapshot(SnapshotKind::Before, "x", Json{{"m", std::string(500, 'x')}}));
        }
    } // the last connection checkpoints the WAL into the file
    {
        std::fstream bytes(file, std::ios::binary | std::ios::in | std::ios::out);
        bytes.seekp(4096); // page 2: a table's b-tree
        const std::string junk(4096, '\xFF');
        bytes.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    std::optional<std::filesystem::path> movedTo;
    auto state = StateDb::openOrReplace(file, kFast, movedTo);
    ASSERT_TRUE(state.has_value()) << state.error().toString();
    EXPECT_TRUE(movedTo.has_value());
    EXPECT_TRUE(state->history().value().empty());
}

TEST(StateDb, AHealthyOrEmptyFileIsKept)
{
    const rmw::test::TempDir dir;
    const auto file = dir.path() / "state.db";
    {
        auto state = StateDb::open(file, kFast);
        ASSERT_TRUE(state.has_value());
        ASSERT_TRUE(state->setMeta("kept", "1").has_value());
    }
    std::optional<std::filesystem::path> movedTo;
    auto state = StateDb::openOrReplace(file, kFast, movedTo);
    ASSERT_TRUE(state.has_value()) << state.error().toString();
    EXPECT_FALSE(movedTo.has_value());
    EXPECT_EQ(state->meta("kept").value(), "1");

    const auto empty = dir.path() / "empty.db";
    std::ofstream(empty, std::ios::binary).close();
    auto fresh = StateDb::openOrReplace(empty, kFast, movedTo);
    ASSERT_TRUE(fresh.has_value()) << fresh.error().toString();
    EXPECT_FALSE(movedTo.has_value()); // a new database, nothing to keep aside
}

} // namespace
