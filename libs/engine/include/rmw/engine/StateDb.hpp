#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/storage/Database.hpp"
#include "rmw/storage/Statement.hpp"

#include <cstdint>
#include <filesystem>
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
};

// An operation that started but has not finished. On startup it is always undone.
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

class StateDb
{
public:
    // Opens (creating if needed) and migrates the schema.
    [[nodiscard]] static Result<StateDb> open(const std::filesystem::path& file,
                                              storage::DatabaseOptions options = {});

    [[nodiscard]] Result<storage::Transaction> transaction() { return storage::Transaction::begin(m_db); }

    // Snapshots.
    [[nodiscard]] Result<std::int64_t> addSnapshot(SnapshotKind kind, std::string_view label,
                                                   const Json& modules);
    [[nodiscard]] Result<std::optional<SnapshotRecord>> snapshot(std::int64_t id);
    [[nodiscard]] Result<std::optional<SnapshotRecord>> baseSnapshot();
    [[nodiscard]] Result<void> deleteSnapshot(std::int64_t id);

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

    [[nodiscard]] storage::Database& database() noexcept { return m_db; }

private:
    explicit StateDb(storage::Database db) noexcept;

    storage::Database m_db;
};

[[nodiscard]] std::int64_t unixNow() noexcept;

} // namespace rmw::engine
