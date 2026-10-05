#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/storage/Database.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct sqlite3_stmt;

namespace rmw::storage
{

// A prepared statement (RAII, move-only). Values always go through bind*(), never into SQL text.
// Parameter and column indexes follow SQLite: parameters start at 1, columns at 0.
class Statement
{
public:
    [[nodiscard]] static Result<Statement> prepare(Database& database, std::string_view sql);

    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    [[nodiscard]] Result<void> bind(int index, std::int64_t value);
    [[nodiscard]] Result<void> bind(int index, double value);
    [[nodiscard]] Result<void> bind(int index, std::string_view text); // copied by SQLite
    [[nodiscard]] Result<void> bindBlob(int index, std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> bindNull(int index);

    // Binds ?1, ?2, ... in order (integers, bool, floating point, text, nullptr for NULL);
    // stops at the first failure.
    template <class... Args>
    [[nodiscard]] Result<void> bindAll(const Args&... args)
    {
        if constexpr (sizeof...(Args) == 0)
        {
            return {};
        }
        else
        {
            int index = 0;
            Result<void> result;
            ((result = result ? bindValue(++index, args) : std::move(result)), ...);
            return result;
        }
    }

    // Advances: true when a row is available, false when the statement is done.
    [[nodiscard]] Result<bool> step();
    // Steps until done (INSERT/UPDATE/DELETE/DDL).
    [[nodiscard]] Result<void> run();
    // Rewinds to run again; bindings are kept until rebound.
    [[nodiscard]] Result<void> reset();

    // Columns of the current row.
    [[nodiscard]] bool isNull(int column) const noexcept;
    [[nodiscard]] std::int64_t columnInt(int column) const noexcept;
    [[nodiscard]] double columnDouble(int column) const noexcept;
    [[nodiscard]] std::string columnText(int column) const;
    [[nodiscard]] std::optional<std::string> columnOptionalText(int column) const;
    [[nodiscard]] std::vector<std::byte> columnBlob(int column) const;

private:
    template <class T>
    [[nodiscard]] Result<void> bindValue(int index, const T& value)
    {
        if constexpr (std::is_same_v<T, std::nullptr_t>)
        {
            return bindNull(index);
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return bind(index, std::int64_t{value ? 1 : 0});
        }
        else if constexpr (std::is_integral_v<T>)
        {
            return bind(index, static_cast<std::int64_t>(value));
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            return bind(index, static_cast<double>(value));
        }
        else
        {
            static_assert(std::is_convertible_v<const T&, std::string_view>, "unsupported bind type");
            return bind(index, std::string_view(value));
        }
    }

    Statement(sqlite3* db, sqlite3_stmt* statement) noexcept;
    [[nodiscard]] Error lastError(std::string_view context, int code) const;
    [[nodiscard]] Result<void> checkBind(int code, int index) const;

    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_statement = nullptr;
};

// BEGIN IMMEDIATE on construction; ROLLBACK on destruction unless commit() succeeded.
class Transaction
{
public:
    [[nodiscard]] static Result<Transaction> begin(Database& database);

    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;

    [[nodiscard]] Result<void> commit();

private:
    explicit Transaction(Database& database) noexcept;

    Database* m_database = nullptr; // null once committed or moved from
};

// A schema step: `sql` runs once, in a transaction, when the database is older than `version`.
struct Migration
{
    int version;
    std::string_view sql;
};

// Brings the schema up to the last migration, tracking progress in PRAGMA user_version.
// Versions must be strictly increasing from 1. A database newer than this build is refused
// (Unsupported) rather than touched.
[[nodiscard]] Result<void> migrate(Database& database, std::span<const Migration> migrations);

// The current PRAGMA user_version.
[[nodiscard]] Result<int> schemaVersion(Database& database);

} // namespace rmw::storage
