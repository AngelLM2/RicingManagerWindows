#include "TempDir.hpp"

#include "rmw/storage/Statement.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace
{

using namespace rmw::storage;

class StatementTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_db.open().has_value());
        ASSERT_TRUE(
            m_db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL, data BLOB);"));
    }

    std::int64_t count()
    {
        auto statement = Statement::prepare(m_db, "SELECT COUNT(*) FROM t;");
        EXPECT_TRUE(statement.has_value());
        EXPECT_TRUE(statement->step().value_or(false));
        return statement->columnInt(0);
    }

    rmw::test::TempDir m_dir;
    Database m_db{m_dir.path() / "test.db", {.synchronous = Synchronous::Off}};
};

TEST_F(StatementTest, BindsAndReadsEveryType)
{
    auto insert = Statement::prepare(m_db, "INSERT INTO t (name, score, data) VALUES (?1, ?2, ?3);");
    ASSERT_TRUE(insert.has_value()) << insert.error().toString();
    const std::array bytes = {std::byte{0}, std::byte{0xFF}, std::byte{7}};
    ASSERT_TRUE(insert->bind(1, std::string_view("夜景 'quoted'; DROP TABLE t;--")));
    ASSERT_TRUE(insert->bind(2, 2.5));
    ASSERT_TRUE(insert->bindBlob(3, bytes));
    ASSERT_TRUE(insert->run());

    auto select = Statement::prepare(m_db, "SELECT id, name, score, data FROM t;");
    ASSERT_TRUE(select.has_value());
    ASSERT_TRUE(select->step().value_or(false));
    EXPECT_EQ(select->columnInt(0), 1);
    EXPECT_EQ(select->columnText(1), "夜景 'quoted'; DROP TABLE t;--");
    EXPECT_DOUBLE_EQ(select->columnDouble(2), 2.5);
    EXPECT_EQ(select->columnBlob(3), std::vector<std::byte>(bytes.begin(), bytes.end()));
    EXPECT_FALSE(select->step().value_or(true));
}

TEST_F(StatementTest, NullsAndEmptyBlobsStayDistinct)
{
    auto insert = Statement::prepare(m_db, "INSERT INTO t (name, data) VALUES (?1, ?2);");
    ASSERT_TRUE(insert.has_value());
    ASSERT_TRUE(insert->bindNull(1));
    ASSERT_TRUE(insert->bindBlob(2, {}));
    ASSERT_TRUE(insert->run());

    auto select = Statement::prepare(m_db, "SELECT name, data IS NULL, length(data) FROM t;");
    ASSERT_TRUE(select.has_value());
    ASSERT_TRUE(select->step().value_or(false));
    EXPECT_TRUE(select->isNull(0));
    EXPECT_FALSE(select->columnOptionalText(0).has_value());
    EXPECT_EQ(select->columnInt(1), 0); // empty blob, not NULL
    EXPECT_EQ(select->columnInt(2), 0);
}

TEST_F(StatementTest, ResetRunsAgainWithNewBindings)
{
    auto insert = Statement::prepare(m_db, "INSERT INTO t (name) VALUES (?1);");
    ASSERT_TRUE(insert.has_value());
    for (const char* name : {"a", "b", "c"})
    {
        ASSERT_TRUE(insert->bind(1, std::string_view(name)));
        ASSERT_TRUE(insert->run());
        ASSERT_TRUE(insert->reset());
    }
    EXPECT_EQ(count(), 3);
}

TEST_F(StatementTest, RejectsInvalidSqlAndMultipleStatements)
{
    EXPECT_FALSE(Statement::prepare(m_db, "SELEC 1;").has_value());
    EXPECT_FALSE(Statement::prepare(m_db, "").has_value());
    const auto twice = Statement::prepare(m_db, "DELETE FROM t; DROP TABLE t;");
    ASSERT_FALSE(twice.has_value());
    EXPECT_EQ(twice.error().code(), rmw::ErrorCode::InvalidArgument);
    EXPECT_TRUE(Statement::prepare(m_db, "SELECT 1;  \n").has_value());
}

TEST_F(StatementTest, ConstraintViolationIsAnError)
{
    ASSERT_TRUE(m_db.execute("INSERT INTO t (id) VALUES (1);"));
    auto insert = Statement::prepare(m_db, "INSERT INTO t (id) VALUES (1);");
    ASSERT_TRUE(insert.has_value());
    const auto result = insert->run();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), rmw::ErrorCode::Database);
    EXPECT_NE(result.error().nativeCode(), 0);
}

TEST_F(StatementTest, TransactionRollsBackUnlessCommitted)
{
    {
        auto transaction = Transaction::begin(m_db);
        ASSERT_TRUE(transaction.has_value());
        ASSERT_TRUE(m_db.execute("INSERT INTO t (name) VALUES ('lost');"));
    }
    EXPECT_EQ(count(), 0);
    {
        auto transaction = Transaction::begin(m_db);
        ASSERT_TRUE(transaction.has_value());
        ASSERT_TRUE(m_db.execute("INSERT INTO t (name) VALUES ('kept');"));
        ASSERT_TRUE(transaction->commit());
        EXPECT_FALSE(transaction->commit().has_value()); // already finished
    }
    EXPECT_EQ(count(), 1);
}

constexpr std::array<Migration, 2> kMigrations = {{
    {1, "CREATE TABLE a (x INTEGER);"},
    {2, "ALTER TABLE a ADD COLUMN y TEXT;"},
}};

TEST(Migrations, ApplyInOrderAndOnlyOnce)
{
    const rmw::test::TempDir dir;
    Database db(dir.path() / "m.db");
    ASSERT_TRUE(db.open().has_value());
    EXPECT_EQ(schemaVersion(db).value(), 0);

    ASSERT_TRUE(migrate(db, std::span(kMigrations).first(1)));
    EXPECT_EQ(schemaVersion(db).value(), 1);
    ASSERT_TRUE(migrate(db, kMigrations));
    EXPECT_EQ(schemaVersion(db).value(), 2);
    ASSERT_TRUE(migrate(db, kMigrations)); // nothing left to do
    EXPECT_TRUE(db.execute("INSERT INTO a (x, y) VALUES (1, 'ok');").has_value());
}

TEST(Migrations, FailedStepLeavesTheVersionUntouched)
{
    const rmw::test::TempDir dir;
    Database db(dir.path() / "m.db");
    ASSERT_TRUE(db.open().has_value());
    const std::array<Migration, 2> broken = {
        {{1, "CREATE TABLE a (x INTEGER);"}, {2, "CREATE TABLE a (x);"}}};
    EXPECT_FALSE(migrate(db, broken).has_value());
    EXPECT_EQ(schemaVersion(db).value(), 1);
}

TEST(Migrations, RefusesNewerDatabasesAndBadLists)
{
    const rmw::test::TempDir dir;
    Database db(dir.path() / "m.db");
    ASSERT_TRUE(db.open().has_value());
    ASSERT_TRUE(db.execute("PRAGMA user_version = 9;"));
    const auto newer = migrate(db, kMigrations);
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code(), rmw::ErrorCode::Unsupported);

    const std::array<Migration, 2> gap = {{{1, "SELECT 1;"}, {3, "SELECT 1;"}}};
    EXPECT_EQ(migrate(db, gap).error().code(), rmw::ErrorCode::InvalidArgument);
}

} // namespace
