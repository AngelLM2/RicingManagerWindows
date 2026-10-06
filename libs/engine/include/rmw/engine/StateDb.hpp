#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/storage/Database.hpp"
#include "rmw/storage/Statement.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// state.db (docs/03 §7): snapshots, the transaction journal, lastApplied per module and the
// history. Only the core writes it; synchronous=FULL so a commit survives a power cut.
namespace rmw::engine
{

using json::Json;

enum class SnapshotKind
{
    Base,   // "my original Windows", captured once on first run
    Before, // taken right before an operation; becomes a history entry when it succeeds
};

struct SnapshotRecord
{
    std::int64_t id = 0;
    SnapshotKind kind = SnapshotKind::Before;
    std::string label;
    std::int64_t createdAt = 0; // Unix seconds
    // {"<module>": {"state": {...}, "fidelity": "exact"|"lossy", "reason": "..."}}
    Json modules = Json::object();
    // Before snapshots: the fields the operation went on to set ({"<module>": {field: value}}), and
    // what the core keeps outside the modules right before it (bars, live wallpaper, schedule...).
    // Empty in snapshots taken before v1.8.
    Json changes = Json::object();
    Json core = Json::object();
};

// An operation that started but has not finished. On startup it is always undone (a restore is
// finished instead).
struct JournalEntry
{
    std::string operation;
    std::int64_t snapshotId = 0;
    Json desired = Json::object(); // {"<module>": {fields being applied}}
    std::int64_t startedAt = 0;
};

struct HistoryEntry
{
    std::int64_t id = 0;
    std::int64_t snapshotId = 0;
    std::string label;
    std::int64_t appliedAt = 0;
};

// Thread-safe: the core's threads share the one connection. Each call holds it for its duration,
// and a transaction for its whole life, so no other thread's write lands inside a transaction (and
// is lost with its rollback) and an inserted row's id is always this call's.
class StateDb
{
public:
    // A write transaction (BEGIN IMMEDIATE; ROLLBACK unless committed). Other threads wait for it.
    class Transaction
    {
    public:
        [[nodiscard]] Result<void> commit() { return m_transaction.commit(); }

    private:
        friend class StateDb;
        Transaction(std::unique_lock<std::recursive_mutex> lock, storage::Transaction transaction) noexcept
            : m_lock(std::move(lock))
            , m_transaction(std::move(transaction))
        {
        }

        std::unique_lock<std::recursive_mutex> m_lock; // first: released after the rollback
        storage::Transaction m_transaction;
    };

    // Opens (creating if needed) and migrates the schema.
    [[nodiscard]] static Result<StateDb> open(const std::filesystem::path& file,
                                              storage::DatabaseOptions options = {});
    // Like open(), but a file that is not a database or is corrupt is moved aside (to
    // "<file>.damaged-<unix time>", with its -wal and -shm) and a new one is created: the core must
    // start anyway. `movedTo` says where the damaged one went.
    [[nodiscard]] static Result<StateDb> openOrReplace(const std::filesystem::path& file,
                                                       storage::DatabaseOptions options,
                                                       std::optional<std::filesystem::path>& movedTo);

    [[nodiscard]] Result<Transaction> transaction();

    // Snapshots.
    [[nodiscard]] Result<std::int64_t> addSnapshot(SnapshotKind kind, std::string_view label,
                                                   const Json& modules);
    // All of `record` but its id; a createdAt of 0 is now.
    [[nodiscard]] Result<std::int64_t> addSnapshot(const SnapshotRecord& record);
    [[nodiscard]] Result<std::optional<SnapshotRecord>> snapshot(std::int64_t id);
    [[nodiscard]] Result<std::optional<SnapshotRecord>> baseSnapshot();
    [[nodiscard]] Result<void> deleteSnapshot(std::int64_t id);
    // Replaces the modules of a snapshot (the base one learns originals of fields set later).
    [[nodiscard]] Result<void> setSnapshotModules(std::int64_t id, const Json& modules);

    // Journal: at most one open entry.
    [[nodiscard]] Result<void> beginJournal(std::string_view operation, std::int64_t snapshotId,
                                            const Json& desired);
    [[nodiscard]] Result<std::optional<JournalEntry>> openJournal();
    [[nodiscard]] Result<void> closeJournal();

    // lastApplied: the fields we set per module and the value each had right after.
    [[nodiscard]] Result<Json> applied(std::string_view module);
    [[nodiscard]] Result<std::vector<std::string>> appliedModules();
    [[nodiscard]] Result<void> setApplied(std::string_view module, const Json& fields); // {} removes

    // History of user operations, newest first; keeps the newest `keep`.
    [[nodiscard]] Result<std::int64_t> addHistory(std::int64_t snapshotId, std::string_view label,
                                                  std::size_t keep);
    [[nodiscard]] Result<std::vector<HistoryEntry>> history();
    [[nodiscard]] Result<std::optional<HistoryEntry>> historyEntry(std::int64_t id);

    // Small key/value flags (e.g. "needs_restore").
    [[nodiscard]] Result<std::optional<std::string>> meta(std::string_view key);
    [[nodiscard]] Result<void> setMeta(std::string_view key, std::optional<std::string_view> value);

    // Whether any stored document (meta values included: the core keeps plans there, such as
    // the slideshow pages it turns) mentions this content-store hash (garbage collection).
    [[nodiscard]] Result<bool> referencesAsset(std::string_view sha256);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_db.path(); }

private:
    explicit StateDb(storage::Database db);

    storage::Database m_db;
    // Recursive: a thread inside its transaction calls the other methods. Owned through a pointer
    // so a StateDb can be moved.
    std::unique_ptr<std::recursive_mutex> m_mutex;
};

[[nodiscard]] std::int64_t unixNow() noexcept;

} // namespace rmw::engine
