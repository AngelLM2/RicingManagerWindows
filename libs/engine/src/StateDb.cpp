#include "rmw/engine/StateDb.hpp"

#include <array>
#include <chrono>
#include <format>

namespace rmw::engine
{

using storage::Statement;

namespace
{

constexpr std::array<storage::Migration, 1> kMigrations = {{
    {1, R"sql(
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) STRICT;

CREATE TABLE snapshots (
    id           INTEGER PRIMARY KEY,
    kind         TEXT NOT NULL CHECK (kind IN ('base', 'before')),
    label        TEXT NOT NULL,
    created_at   INTEGER NOT NULL,
    modules_json TEXT NOT NULL
) STRICT;
CREATE UNIQUE INDEX snapshots_one_base ON snapshots (kind) WHERE kind = 'base';

CREATE TABLE journal (
    id           INTEGER PRIMARY KEY CHECK (id = 1),
    operation    TEXT NOT NULL,
    snapshot_id  INTEGER NOT NULL REFERENCES snapshots (id),
    desired_json TEXT NOT NULL,
    started_at   INTEGER NOT NULL
) STRICT;

CREATE TABLE applied (
    module      TEXT PRIMARY KEY,
    fields_json TEXT NOT NULL,
    updated_at  INTEGER NOT NULL
) STRICT;

CREATE TABLE history (
    id          INTEGER PRIMARY KEY,
    snapshot_id INTEGER NOT NULL UNIQUE REFERENCES snapshots (id) ON DELETE CASCADE,
    label       TEXT NOT NULL,
    applied_at  INTEGER NOT NULL
) STRICT;
)sql"},
}};

std::string_view kindName(SnapshotKind kind) noexcept
{
    return kind == SnapshotKind::Base ? "base" : "before";
}

// Documents in state.db are ours, but the file can be damaged: parse them like any input.
Result<Json> parseStored(const std::string& text, std::string_view what)
{
    json::ParseLimits limits;
    limits.maxBytes = 64u << 20;
    limits.maxElements = 1'000'000;
    return json::parse(text, limits).transform_error([&](Error error) {
        return std::move(error).withContext(std::format("state.db {}", what));
    });
}

template <class... Args>
Result<Statement> query(storage::Database& db, std::string_view sql, const Args&... args)
{
    auto statement = Statement::prepare(db, sql);
    if (!statement)
    {
        return statement;
    }
    if (auto bound = statement->bindAll(args...); !bound)
    {
        return std::unexpected(std::move(bound.error()));
    }
    return statement;
}

template <class... Args>
Result<void> execute(storage::Database& db, std::string_view sql, const Args&... args)
{
    auto statement = query(db, sql, args...);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    return statement->run();
}

Result<SnapshotRecord> readSnapshot(const Statement& row)
{
    SnapshotRecord record;
    record.id = row.columnInt(0);
    record.kind = row.columnText(1) == "base" ? SnapshotKind::Base : SnapshotKind::Before;
    record.label = row.columnText(2);
    record.createdAt = row.columnInt(3);
    auto modules = parseStored(row.columnText(4), "snapshot");
    if (!modules)
    {
        return std::unexpected(std::move(modules.error()));
    }
    if (!modules->is_object())
    {
        return fail(ErrorCode::InvalidData, "state.db snapshot is not an object");
    }
    record.modules = std::move(*modules);
    return record;
}

// Runs a one-row query and maps the row, or nullopt when there is none.
template <class T, class Map>
Result<std::optional<T>> single(Result<Statement> statement, Map map)
{
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
        return std::optional<T>{};
    }
    return map(*statement).transform([](T value) { return std::optional<T>(std::move(value)); });
}

} // namespace

std::int64_t unixNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

StateDb::StateDb(storage::Database db) noexcept
    : m_db(std::move(db))
{
}

Result<StateDb> StateDb::open(const std::filesystem::path& file, storage::DatabaseOptions options)
{
    storage::Database db(file, options);
    if (auto opened = db.open(); !opened)
    {
        return std::unexpected(std::move(opened.error()));
    }
    if (auto migrated = storage::migrate(db, kMigrations); !migrated)
    {
        return std::unexpected(std::move(migrated.error()));
    }
    return StateDb(std::move(db));
}

// ---- snapshots ------------------------------------------------------------------------------

Result<std::int64_t> StateDb::addSnapshot(SnapshotKind kind, std::string_view label, const Json& modules)
{
    const std::string text = json::dump(modules);
    if (auto inserted = execute(
            m_db, "INSERT INTO snapshots (kind, label, created_at, modules_json) VALUES (?1, ?2, ?3, ?4);",
            kindName(kind), label, unixNow(), text);
        !inserted)
    {
        return std::unexpected(std::move(inserted.error()));
    }
    return m_db.lastInsertRowId();
}

Result<std::optional<SnapshotRecord>> StateDb::snapshot(std::int64_t id)
{
    return single<SnapshotRecord>(
        query(m_db, "SELECT id, kind, label, created_at, modules_json FROM snapshots WHERE id = ?1;", id),
        readSnapshot);
}

Result<std::optional<SnapshotRecord>> StateDb::baseSnapshot()
{
    return single<SnapshotRecord>(
        query(m_db, "SELECT id, kind, label, created_at, modules_json FROM snapshots WHERE kind = 'base';"),
        readSnapshot);
}

Result<void> StateDb::deleteSnapshot(std::int64_t id)
{
    return execute(m_db, "DELETE FROM snapshots WHERE id = ?1 AND kind = 'before';", id);
}

// ---- journal --------------------------------------------------------------------------------

Result<void> StateDb::beginJournal(std::string_view operation, std::int64_t snapshotId, const Json& desired)
{
    const std::string text = json::dump(desired);
    // id = 1 is the only allowed row: a second open entry fails on the primary key.
    return execute(m_db,
                   "INSERT INTO journal (id, operation, snapshot_id, desired_json, started_at) VALUES (1, "
                   "?1, ?2, ?3, ?4);",
                   operation, snapshotId, text, unixNow());
}

Result<std::optional<JournalEntry>> StateDb::openJournal()
{
    return single<JournalEntry>(
        query(m_db, "SELECT operation, snapshot_id, desired_json, started_at FROM journal WHERE id = 1;"),
        [](const Statement& row) -> Result<JournalEntry> {
            JournalEntry entry;
            entry.operation = row.columnText(0);
            entry.snapshotId = row.columnInt(1);
            auto desired = parseStored(row.columnText(2), "journal");
            if (!desired)
            {
                return std::unexpected(std::move(desired.error()));
            }
            entry.desired = std::move(*desired);
            entry.startedAt = row.columnInt(3);
            return entry;
        });
}

Result<void> StateDb::closeJournal()
{
    return execute(m_db, "DELETE FROM journal WHERE id = 1;");
}

// ---- applied --------------------------------------------------------------------------------

Result<Json> StateDb::applied(std::string_view module)
{
    auto fields =
        single<Json>(query(m_db, "SELECT fields_json FROM applied WHERE module = ?1;", module),
                     [](const Statement& row) { return parseStored(row.columnText(0), "applied"); });
    if (!fields)
    {
        return std::unexpected(std::move(fields.error()));
    }
    if (!*fields || !(*fields)->is_object())
    {
        return Json::object();
    }
    return std::move(**fields);
}

Result<std::vector<std::string>> StateDb::appliedModules()
{
    auto statement = query(m_db, "SELECT module FROM applied ORDER BY module;");
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<std::string> modules;
    while (true)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return modules;
        }
        modules.push_back(statement->columnText(0));
    }
}

Result<void> StateDb::setApplied(std::string_view module, const Json& fields)
{
    if (!fields.is_object() || fields.empty())
    {
        return execute(m_db, "DELETE FROM applied WHERE module = ?1;", module);
    }
    const std::string text = json::dump(fields);
    return execute(m_db,
                   "INSERT INTO applied (module, fields_json, updated_at) VALUES (?1, ?2, ?3) "
                   "ON CONFLICT (module) DO UPDATE SET fields_json = excluded.fields_json, "
                   "updated_at = excluded.updated_at;",
                   module, text, unixNow());
}

// ---- history --------------------------------------------------------------------------------

Result<std::int64_t> StateDb::addHistory(std::int64_t snapshotId, std::string_view label, std::size_t keep)
{
    if (auto inserted =
            execute(m_db, "INSERT INTO history (snapshot_id, label, applied_at) VALUES (?1, ?2, ?3);",
                    snapshotId, label, unixNow());
        !inserted)
    {
        return std::unexpected(std::move(inserted.error()));
    }
    const std::int64_t id = m_db.lastInsertRowId();
    // Dropping the snapshot cascades to its history row.
    if (auto trimmed = execute(m_db,
                               "DELETE FROM snapshots WHERE id IN "
                               "(SELECT snapshot_id FROM history ORDER BY id DESC LIMIT -1 OFFSET ?1);",
                               static_cast<std::int64_t>(keep));
        !trimmed)
    {
        return std::unexpected(std::move(trimmed.error()));
    }
    return id;
}

Result<std::vector<HistoryEntry>> StateDb::history()
{
    auto statement = query(m_db, "SELECT id, snapshot_id, label, applied_at FROM history ORDER BY id DESC;");
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<HistoryEntry> entries;
    while (true)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return entries;
        }
        entries.push_back({statement->columnInt(0), statement->columnInt(1), statement->columnText(2),
                           statement->columnInt(3)});
    }
}

Result<std::optional<HistoryEntry>> StateDb::historyEntry(std::int64_t id)
{
    return single<HistoryEntry>(
        query(m_db, "SELECT id, snapshot_id, label, applied_at FROM history WHERE id = ?1;", id),
        [](const Statement& row) -> Result<HistoryEntry> {
            return HistoryEntry{row.columnInt(0), row.columnInt(1), row.columnText(2), row.columnInt(3)};
        });
}

// ---- meta and assets ------------------------------------------------------------------------

Result<std::optional<std::string>> StateDb::meta(std::string_view key)
{
    return single<std::string>(query(m_db, "SELECT value FROM meta WHERE key = ?1;", key),
                               [](const Statement& row) -> Result<std::string> { return row.columnText(0); });
}

Result<void> StateDb::setMeta(std::string_view key, std::optional<std::string_view> value)
{
    if (!value)
    {
        return execute(m_db, "DELETE FROM meta WHERE key = ?1;", key);
    }
    return execute(m_db,
                   "INSERT INTO meta (key, value) VALUES (?1, ?2) ON CONFLICT (key) DO UPDATE SET value = "
                   "excluded.value;",
                   key, *value);
}

Result<bool> StateDb::referencesAsset(std::string_view sha256)
{
    auto found =
        single<bool>(query(m_db,
                           "SELECT EXISTS (SELECT 1 FROM snapshots WHERE instr(modules_json, ?1) > 0) "
                           "OR EXISTS (SELECT 1 FROM applied WHERE instr(fields_json, ?1) > 0) "
                           "OR EXISTS (SELECT 1 FROM journal WHERE instr(desired_json, ?1) > 0);",
                           sha256),
                     [](const Statement& row) -> Result<bool> { return row.columnInt(0) != 0; });
    if (!found)
    {
        return std::unexpected(std::move(found.error()));
    }
    return found->value_or(true);
}

} // namespace rmw::engine
