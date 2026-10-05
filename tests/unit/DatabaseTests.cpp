#include "TempDir.hpp"

#include "rmw/storage/Database.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

namespace
{

std::int64_t queryInt(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        return -1;
    }
    std::int64_t value = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

TEST(Database, CreatesDirectoryAndOpens)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / "nested" / "state.db");
    ASSERT_TRUE(db.open().has_value());
    EXPECT_TRUE(db.isOpen());
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "nested" / "state.db"));
}

TEST(Database, OpensUnicodePaths)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / L"夜景 Índice" / L"state.db");
    const auto opened = db.open();
    ASSERT_TRUE(opened.has_value()) << opened.error().toString();
}

TEST(Database, AppliesDurabilityAndSafetyPragmas)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / "state.db");
    ASSERT_TRUE(db.open().has_value());
    EXPECT_EQ(queryInt(db.handle(), "PRAGMA synchronous;"), 2); // FULL
    EXPECT_EQ(queryInt(db.handle(), "PRAGMA foreign_keys;"), 1);

    int defensive = 0;
    sqlite3_db_config(db.handle(), SQLITE_DBCONFIG_DEFENSIVE, -1, &defensive);
    EXPECT_EQ(defensive, 1);
}

TEST(Database, NormalSynchronousForCaches)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / "library.db", {.synchronous = rmw::storage::Synchronous::Normal});
    ASSERT_TRUE(db.open().has_value());
    EXPECT_EQ(queryInt(db.handle(), "PRAGMA synchronous;"), 1); // NORMAL
}

TEST(Database, ExecuteAndTransactions)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / "state.db");
    ASSERT_TRUE(db.open().has_value());
    ASSERT_TRUE(db.execute("CREATE TABLE t(v INTEGER);").has_value());

    ASSERT_TRUE(db.beginTransaction().has_value());
    ASSERT_TRUE(db.execute("INSERT INTO t VALUES (1);").has_value());
    ASSERT_TRUE(db.rollback().has_value());
    EXPECT_EQ(queryInt(db.handle(), "SELECT COUNT(*) FROM t;"), 0);

    ASSERT_TRUE(db.beginTransaction().has_value());
    ASSERT_TRUE(db.execute("INSERT INTO t VALUES (2);").has_value());
    ASSERT_TRUE(db.commit().has_value());
    EXPECT_EQ(queryInt(db.handle(), "SELECT COUNT(*) FROM t;"), 1);
}

TEST(Database, ReportsSqlErrors)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database db(dir.path() / "state.db");
    ASSERT_TRUE(db.open().has_value());
    const auto result = db.execute("THIS IS NOT SQL;");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), rmw::ErrorCode::Database);
}

TEST(Database, RejectsUseBeforeOpenAndEmptyPath)
{
    rmw::storage::Database closed("unused.db");
    EXPECT_FALSE(closed.execute("SELECT 1;").has_value());

    rmw::storage::Database empty{std::filesystem::path{}};
    EXPECT_FALSE(empty.open().has_value());
}

TEST(Database, MoveTransfersOwnership)
{
    const rmw::test::TempDir dir;
    rmw::storage::Database first(dir.path() / "state.db");
    ASSERT_TRUE(first.open().has_value());

    rmw::storage::Database second(std::move(first));
    EXPECT_FALSE(first.isOpen()); // NOLINT(bugprone-use-after-move): verifying moved-from state
    EXPECT_TRUE(second.isOpen());
    EXPECT_TRUE(second.execute("SELECT 1;").has_value());
}

} // namespace
