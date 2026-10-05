#include "rmw/storage/Database.hpp"

#include "rmw/base/Log.hpp"

#include <sqlite3.h>

#include <format>
#include <string>
#include <utility>

namespace rmw::storage
{
namespace
{

// std::filesystem::path is UTF-16 on Windows; sqlite3_open_v2 expects UTF-8.
std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string_view synchronousPragma(Synchronous mode) noexcept
{
    switch (mode)
    {
    case Synchronous::Off:
        return "PRAGMA synchronous = OFF;";
    case Synchronous::Normal:
        return "PRAGMA synchronous = NORMAL;";
    case Synchronous::Full:
        return "PRAGMA synchronous = FULL;";
    }
    return "PRAGMA synchronous = FULL;";
}

} // namespace

Database::Database(std::filesystem::path path, DatabaseOptions options)
    : m_path(std::move(path))
    , m_options(options)
{
}

Database::~Database()
{
    close();
}

Database::Database(Database&& other) noexcept
    : m_path(std::move(other.m_path))
    , m_options(other.m_options)
    , m_db(std::exchange(other.m_db, nullptr))
{
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_path = std::move(other.m_path);
        m_options = other.m_options;
        m_db = std::exchange(other.m_db, nullptr);
    }
    return *this;
}

Result<void> Database::open()
{
    if (isOpen())
    {
        return {};
    }
    if (m_path.empty())
    {
        return fail(ErrorCode::InvalidArgument, "database path is empty");
    }

    if (const auto parent = m_path.parent_path(); !parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return fail(ErrorCode::Io, std::format("cannot create database directory: {}", ec.message()),
                        ec.value());
        }
    }

    // NOFOLLOW: refuse symbolic links as the database path.
    constexpr int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    const int rc = sqlite3_open_v2(pathToUtf8(m_path).c_str(), &m_db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        // Even on failure SQLite may allocate a handle that must be closed.
        Error error = lastError("open", rc);
        close();
        return std::unexpected(std::move(error));
    }

    if (auto configured = configure(); !configured)
    {
        close();
        return std::unexpected(std::move(configured.error()).withContext("configure database"));
    }
    return {};
}

void Database::close() noexcept
{
    if (m_db == nullptr)
    {
        return;
    }
    // close_v2 defers the real close until outstanding statements are finalized.
    if (sqlite3_close_v2(m_db) != SQLITE_OK)
    {
        log::error("failed to close SQLite database");
    }
    m_db = nullptr;
}

Result<void> Database::execute(std::string_view sql)
{
    if (!isOpen())
    {
        return fail(ErrorCode::InvalidArgument, "database is not open");
    }
    if (sql.empty())
    {
        return fail(ErrorCode::InvalidArgument, "empty SQL");
    }

    // sqlite3_exec needs a NUL-terminated string; string_view does not guarantee one.
    const std::string statement(sql);
    const int rc = sqlite3_exec(m_db, statement.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
    {
        return std::unexpected(lastError("execute", rc));
    }
    return {};
}

Result<void> Database::beginTransaction()
{
    // IMMEDIATE takes the write lock up front instead of at the first write.
    return execute("BEGIN IMMEDIATE TRANSACTION;");
}

Result<void> Database::commit()
{
    return execute("COMMIT;");
}

Result<void> Database::rollback()
{
    return execute("ROLLBACK;");
}

bool Database::isOpen() const noexcept
{
    return m_db != nullptr;
}

sqlite3* Database::handle() const noexcept
{
    return m_db;
}

const std::filesystem::path& Database::path() const noexcept
{
    return m_path;
}

const DatabaseOptions& Database::options() const noexcept
{
    return m_options;
}

Result<void> Database::configure()
{
    if (const int rc = sqlite3_extended_result_codes(m_db, 1); rc != SQLITE_OK)
    {
        return std::unexpected(lastError("enable extended result codes", rc));
    }
    if (const int rc = sqlite3_busy_timeout(m_db, static_cast<int>(m_options.busyTimeout.count()));
        rc != SQLITE_OK)
    {
        return std::unexpected(lastError("set busy timeout", rc));
    }

    // Hardening: SQL cannot corrupt the file on purpose, and the schema is not trusted to run
    // functions with side effects. Extension loading stays disabled (SQLite default).
    if (const int rc = sqlite3_db_config(m_db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr); rc != SQLITE_OK)
    {
        return std::unexpected(lastError("enable defensive mode", rc));
    }
    if (const int rc = sqlite3_db_config(m_db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr); rc != SQLITE_OK)
    {
        return std::unexpected(lastError("disable trusted schema", rc));
    }

    // Foreign keys must be enabled per connection.
    if (auto r = execute("PRAGMA foreign_keys = ON;"); !r)
    {
        return r;
    }
    if (m_options.writeAheadLog)
    {
        if (auto r = execute("PRAGMA journal_mode = WAL;"); !r)
        {
            return r;
        }
    }
    return execute(synchronousPragma(m_options.synchronous));
}

Error Database::lastError(std::string_view context, int code) const
{
    const char* message = m_db != nullptr ? sqlite3_errmsg(m_db) : sqlite3_errstr(code);
    return Error(ErrorCode::Database, message != nullptr ? message : "unknown SQLite error", code)
        .withContext(context);
}

} // namespace rmw::storage
