#pragma once

#include "rmw/base/Error.hpp"

#include <chrono>
#include <filesystem>
#include <string_view>

// Forward declaration: sqlite3.h is included only in Database.cpp.
struct sqlite3;

namespace rmw::storage
{

// How hard SQLite works to survive a power loss (docs/03 §7).
enum class Synchronous
{
    Off,    // tests only
    Normal, // caches (library.db): in WAL mode the last commit may be lost on power failure
    Full,   // state.db and the transaction journal: commits are durable
};

struct DatabaseOptions
{
    Synchronous synchronous = Synchronous::Full;
    std::chrono::milliseconds busyTimeout{5000};
    bool writeAheadLog = true;
};

// Owns one SQLite connection (RAII, move-only).
class Database
{
public:
    // Does not open the file; call open().
    explicit Database(std::filesystem::path path, DatabaseOptions options = {});
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Opens or creates the file (and its parent directory) and configures the connection.
    // Opening an already-open database succeeds without doing anything.
    [[nodiscard]] Result<void> open();
    void close() noexcept;

    // Runs SQL that returns no rows (DDL, PRAGMA, fixed statements). Never build SQL from
    // untrusted input here: values go through prepared statements (added in M1).
    [[nodiscard]] Result<void> execute(std::string_view sql);

    [[nodiscard]] Result<void> beginTransaction(); // BEGIN IMMEDIATE
    [[nodiscard]] Result<void> commit();
    [[nodiscard]] Result<void> rollback();

    [[nodiscard]] bool isOpen() const noexcept;
    // Borrowed pointer; never close it.
    [[nodiscard]] sqlite3* handle() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const DatabaseOptions& options() const noexcept;

private:
    [[nodiscard]] Result<void> configure();
    [[nodiscard]] Error lastError(std::string_view context, int code) const;

    std::filesystem::path m_path;
    DatabaseOptions m_options;
    sqlite3* m_db = nullptr;
};

} // namespace rmw::storage
