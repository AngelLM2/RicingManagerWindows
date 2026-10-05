#include "rmw/storage/Statement.hpp"

#include "rmw/base/CheckedCast.hpp"
#include "rmw/base/Log.hpp"

#include <sqlite3.h>

#include <format>
#include <utility>

namespace rmw::storage
{

// ---- Statement ------------------------------------------------------------------------------

Statement::Statement(sqlite3* db, sqlite3_stmt* statement) noexcept
    : m_db(db)
    , m_statement(statement)
{
}

Result<Statement> Statement::prepare(Database& database, std::string_view sql)
{
    if (!database.isOpen())
    {
        return fail(ErrorCode::InvalidArgument, "database is not open");
    }
    const auto length = checkedCast<int>(sql.size());
    if (!length)
    {
        return fail(ErrorCode::InvalidArgument, "SQL text too long");
    }

    sqlite3_stmt* statement = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v3(database.handle(), sql.data(), *length, SQLITE_PREPARE_PERSISTENT,
                                      &statement, &tail);
    if (rc != SQLITE_OK)
    {
        sqlite3_finalize(statement);
        return std::unexpected(Error(ErrorCode::Database, sqlite3_errmsg(database.handle()), rc)
                                   .withContext(std::format("prepare '{}'", sql.substr(0, 80))));
    }
    if (statement == nullptr)
    {
        return fail(ErrorCode::InvalidArgument, "SQL contains no statement");
    }
    if (tail != nullptr &&
        std::string_view(tail, sql.data() + sql.size() - tail).find_first_not_of(" \t\r\n;") !=
            std::string_view::npos)
    {
        sqlite3_finalize(statement);
        return fail(ErrorCode::InvalidArgument, "prepare takes exactly one SQL statement");
    }
    return Statement(database.handle(), statement);
}

Statement::~Statement()
{
    sqlite3_finalize(m_statement);
}

Statement::Statement(Statement&& other) noexcept
    : m_db(std::exchange(other.m_db, nullptr))
    , m_statement(std::exchange(other.m_statement, nullptr))
{
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other)
    {
        sqlite3_finalize(m_statement);
        m_db = std::exchange(other.m_db, nullptr);
        m_statement = std::exchange(other.m_statement, nullptr);
    }
    return *this;
}

Result<void> Statement::checkBind(int code, int index) const
{
    if (code != SQLITE_OK)
    {
        return std::unexpected(lastError(std::format("bind parameter {}", index), code));
    }
    return {};
}

Result<void> Statement::bind(int index, std::int64_t value)
{
    return checkBind(sqlite3_bind_int64(m_statement, index, value), index);
}

Result<void> Statement::bind(int index, double value)
{
    return checkBind(sqlite3_bind_double(m_statement, index, value), index);
}

Result<void> Statement::bind(int index, std::string_view text)
{
    return checkBind(
        sqlite3_bind_text64(m_statement, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8),
        index);
}

Result<void> Statement::bindBlob(int index, std::span<const std::byte> bytes)
{
    // A null data pointer would bind NULL; an empty blob must stay a zero-length blob.
    static constexpr std::byte empty{};
    const void* data = bytes.empty() ? &empty : bytes.data();
    return checkBind(sqlite3_bind_blob64(m_statement, index, data, bytes.size(), SQLITE_TRANSIENT), index);
}

Result<void> Statement::bindNull(int index)
{
    return checkBind(sqlite3_bind_null(m_statement, index), index);
}

Result<bool> Statement::step()
{
    const int rc = sqlite3_step(m_statement);
    if (rc == SQLITE_ROW)
    {
        return true;
    }
    if (rc == SQLITE_DONE)
    {
        return false;
    }
    return std::unexpected(lastError("step", rc));
}

Result<void> Statement::run()
{
    while (true)
    {
        auto row = step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return {};
        }
    }
}

Result<void> Statement::reset()
{
    if (const int rc = sqlite3_reset(m_statement); rc != SQLITE_OK)
    {
        return std::unexpected(lastError("reset", rc));
    }
    return {};
}

bool Statement::isNull(int column) const noexcept
{
    return sqlite3_column_type(m_statement, column) == SQLITE_NULL;
}

std::int64_t Statement::columnInt(int column) const noexcept
{
    return sqlite3_column_int64(m_statement, column);
}

double Statement::columnDouble(int column) const noexcept
{
    return sqlite3_column_double(m_statement, column);
}

std::string Statement::columnText(int column) const
{
    const auto* text = sqlite3_column_text(m_statement, column);
    const int bytes = sqlite3_column_bytes(m_statement, column);
    if (text == nullptr || bytes <= 0)
    {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

std::optional<std::string> Statement::columnOptionalText(int column) const
{
    if (isNull(column))
    {
        return std::nullopt;
    }
    return columnText(column);
}

std::vector<std::byte> Statement::columnBlob(int column) const
{
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(m_statement, column));
    const int bytes = sqlite3_column_bytes(m_statement, column);
    if (data == nullptr || bytes <= 0)
    {
        return {};
    }
    return {data, data + bytes};
}

Error Statement::lastError(std::string_view context, int code) const
{
    const char* message = m_db != nullptr ? sqlite3_errmsg(m_db) : sqlite3_errstr(code);
    return Error(ErrorCode::Database, message != nullptr ? message : "unknown SQLite error", code)
        .withContext(context);
}

// ---- Transaction ----------------------------------------------------------------------------

Transaction::Transaction(Database& database) noexcept
    : m_database(&database)
{
}

Result<Transaction> Transaction::begin(Database& database)
{
    if (auto begun = database.beginTransaction(); !begun)
    {
        return std::unexpected(std::move(begun.error()));
    }
    return Transaction(database);
}

Transaction::~Transaction()
{
    if (m_database != nullptr)
    {
        if (auto rolledBack = m_database->rollback(); !rolledBack)
        {
            log::error("rollback failed: {}", rolledBack.error().toString());
        }
    }
}

Transaction::Transaction(Transaction&& other) noexcept
    : m_database(std::exchange(other.m_database, nullptr))
{
}

Result<void> Transaction::commit()
{
    if (m_database == nullptr)
    {
        return fail(ErrorCode::InvalidArgument, "transaction already finished");
    }
    auto committed = m_database->commit();
    if (committed)
    {
        m_database = nullptr;
    }
    return committed;
}

// ---- migrations ----------------------------------------------------------------------------

Result<int> schemaVersion(Database& database)
{
    auto statement = Statement::prepare(database, "PRAGMA user_version;");
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return fail(ErrorCode::Database, "PRAGMA user_version returned no row");
    }
    return static_cast<int>(statement->columnInt(0));
}

Result<void> migrate(Database& database, std::span<const Migration> migrations)
{
    int previous = 0;
    for (const auto& migration : migrations)
    {
        if (migration.version != previous + 1)
        {
            return fail(ErrorCode::InvalidArgument,
                        std::format("migration versions must count up from 1 (found {} after {})",
                                    migration.version, previous));
        }
        previous = migration.version;
    }

    auto current = schemaVersion(database);
    if (!current)
    {
        return std::unexpected(std::move(current.error()));
    }
    const int latest = previous;
    if (*current > latest)
    {
        return fail(ErrorCode::Unsupported,
                    std::format("{} was written by a newer version (schema {}, this build knows {})",
                                database.path().filename().string(), *current, latest));
    }

    for (const auto& migration : migrations)
    {
        if (migration.version <= *current)
        {
            continue;
        }
        auto transaction = Transaction::begin(database);
        if (!transaction)
        {
            return std::unexpected(std::move(transaction.error()));
        }
        if (auto applied = database.execute(migration.sql); !applied)
        {
            return std::unexpected(
                std::move(applied.error()).withContext(std::format("migration {}", migration.version)));
        }
        if (auto versioned = database.execute(std::format("PRAGMA user_version = {};", migration.version));
            !versioned)
        {
            return versioned;
        }
        if (auto committed = transaction->commit(); !committed)
        {
            return committed;
        }
    }
    return {};
}

} // namespace rmw::storage
