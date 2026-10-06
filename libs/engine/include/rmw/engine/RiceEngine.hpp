#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/engine/ContentStore.hpp"
#include "rmw/engine/ModuleCatalog.hpp"
#include "rmw/engine/StateDb.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rmw::engine
{

struct SkippedField
{
    std::string module;
    std::string field;
    std::string reason;
};

// A module a restore could not put back (the others went on): it stays as it was, and stays ours.
struct FailedModule
{
    std::string module;
    std::string reason;
};

struct OperationReport
{
    std::int64_t historyId = 0;       // 0 when nothing changed
    std::vector<std::string> modules; // modules that were changed
    std::vector<SkippedField> skipped;
    std::vector<std::string> warnings;
    std::vector<FailedModule> failed; // restores only
    // restoreHistory(): what the core kept outside the modules right before that entry (setCoreState);
    // empty for entries older than v1.8.
    Json core = Json::object();
};

struct RestoreOptions
{
    // Also restore fields the user changed by other means after we applied them.
    bool force = false;
};

// Transactional apply/undo (docs/03 §5). Not thread-safe: the core runs one operation at a
// time and answers `busy` to the rest.
//
//   1. compute what each module should change (nothing touched yet)
//   2. capture the current state of those modules
//   3. durably record that snapshot and the journal entry ("in progress")
//   4. apply module by module in dependency order; on any failure undo every touched module,
//      the failed one included, in reverse order. A restore goes on instead: the module that
//      failed stays as it is (reported in `failed`) and the others still come back, since going
//      back to the original is never better undone (ADR-0023)
//   5. record lastApplied per field and the history entry; close the journal
//
// A crash between 3 and 5 leaves the journal open; recover() undoes it on the next start (a
// restore is finished instead). An undo is best effort and always closes the journal: what could
// not be undone stays as the operation left it, counts as ours (a restore puts it back), and the
// "needs_restore" flag says so until a restore of the original works.
class RiceEngine
{
public:
    static constexpr std::size_t kHistoryLimit = 20;

    RiceEngine(StateDb& state, ContentStore& store, const ModuleCatalog& catalog, IModuleRunner& runner);

    // Undoes an operation interrupted by a crash (an interrupted restore is finished). True when
    // there was one. Fails only when state.db itself does.
    [[nodiscard]] Result<bool> recover();
    // Forgets an interrupted operation without undoing it (safe mode: the undo may be what crashes).
    // What it changed stays and counts as ours; "needs_restore" is cleared. True when there was one.
    [[nodiscard]] Result<bool> discardJournal();

    // Captures "my original Windows" once (first run). True when it was captured now. With a backup
    // file (setOriginalBackup), a state.db that lost its original gets it back from there instead.
    [[nodiscard]] Result<bool> ensureBaseSnapshot();
    // A copy of the original snapshot outside state.db, written whenever it changes: a damaged or
    // lost state.db must not make the next apply take an already riced Windows for the original.
    void setOriginalBackup(std::filesystem::path file) { m_originalBackup = std::move(file); }
    // What the core keeps outside the modules (bars, live wallpaper, schedule...), saved with the
    // snapshot taken before each operation: going back to a history entry brings it back too.
    void setCoreState(std::function<Json()> provider) { m_coreState = std::move(provider); }
    // The base snapshot ("my original Windows"), brought back from the backup file when state.db
    // lost it; nullopt when there is none (nothing was ever applied).
    [[nodiscard]] Result<std::optional<SnapshotRecord>> originalSnapshot();

    // `changesElsewhere`: the rice also changes what no module holds (Ricing Manager's bars, which the
    // core runs): a rice that changes nothing of the modules is then no mistake but an empty report
    // (historyId 0). Without it, such a rice fails: it would do nothing at all.
    [[nodiscard]] Result<OperationReport> apply(const domain::Rice& rice, const std::filesystem::path& packageRoot,
                                                bool changesElsewhere = false);

    // Puts back the original value of every field we changed, except fields the user changed
    // after us (listed in `skipped`) unless `force`. Fails only when no module came back. A field
    // Windows does not show now (a monitor not connected) stays ours, listed in `skipped`.
    [[nodiscard]] Result<OperationReport> restoreOriginal(const RestoreOptions& options = {});

    // Returns to how things were right before history entry `historyId`, same field rules: the
    // fields we manage now and the ones that operation changed (going back past a restore brings
    // the rice it took away back).
    [[nodiscard]] Result<OperationReport> restoreHistory(std::int64_t historyId,
                                                         const RestoreOptions& options = {});

    [[nodiscard]] Result<bool> needsRestore();

    // Like apply(), but no history entry: automatic changes (schedule, rules) are not something
    // the user would go back to (docs/03 §5). Modules Windows already shows as desired are left
    // alone, so a schedule check after resume does not re-apply the theme.
    [[nodiscard]] Result<OperationReport> applyAutomatic(const domain::Rice& rice,
                                                         const std::filesystem::path& packageRoot,
                                                         const std::string& label, bool changesElsewhere = false);

    // A few fields of one module chosen by the core itself (a slideshow turning a page): an
    // automatic change like applyAutomatic().
    [[nodiscard]] Result<OperationReport> applyFields(std::string_view moduleId, const Json& fields,
                                                      const std::string& label);

    // Fields of a module that Windows no longer shows as we set them: someone changed them by
    // other means (docs/03 §5.2). Read-only.
    [[nodiscard]] Result<std::vector<std::string>> driftedFields(std::string_view moduleId);
    // The user wins: stop managing these fields (they are not restored or reapplied anymore).
    [[nodiscard]] Result<void> detach(std::string_view moduleId, const std::vector<std::string>& fields);
    // Explorer restarted or a monitor came back: set again only what is still ours and differs.
    // No history. `modules` limits it (empty = every module).
    [[nodiscard]] Result<OperationReport> reapplyManaged(std::span<const std::string> modules = {});

    // Test hook (system tests only): called after each module's apply succeeds, before the next.
    void setAfterModuleHook(std::function<void(std::string_view module)> hook)
    {
        m_afterModule = std::move(hook);
    }

private:
    struct Step
    {
        const IModuleLogic* module = nullptr;
        Json desired;
        std::optional<CaptureResult> current; // captured already (restores)
    };

    enum class AppliedUpdate
    {
        Merge,  // the fields are now ours
        Remove, // the fields are back to the original: no longer ours
    };

    // `automatic`: no history entry, and modules already as desired are skipped.
    [[nodiscard]] Result<OperationReport> run(std::string_view operation, const std::string& label,
                                              std::vector<Step> steps, AppliedUpdate update,
                                              OperationReport report, bool automatic = false);
    // Step 5: what is ours now per field, the history entry, the journal closed — one transaction.
    // Modules in report.failed are left as they were. Returns the history id (0 when automatic).
    [[nodiscard]] Result<std::int64_t> recordOperation(const std::string& label, std::span<const Step> steps,
                                                       AppliedUpdate update, std::int64_t snapshotId,
                                                       bool automatic, OperationReport& report);
    [[nodiscard]] Result<std::vector<Step>> stepsFromRice(const domain::Rice& rice,
                                                          const std::filesystem::path& packageRoot);
    [[nodiscard]] Result<OperationReport> restoreTo(const SnapshotRecord& target, std::string_view operation,
                                                    const std::string& label, const RestoreOptions& options,
                                                    AppliedUpdate update);
    // Puts back, in reverse order, the fields `desired` was setting; returns the modules that failed.
    [[nodiscard]] std::vector<FailedModule> undo(const Json& beforeModules, const Json& desired,
                                                 const std::vector<std::string>& touched);
    // After an undo (or instead of one: `discarded`): what was not undone becomes ours (a restore
    // puts it back), the journal is closed, and when something was not undone its snapshot stays as
    // a history entry and needs_restore is set (cleared instead when the user discarded it).
    // entry.snapshotId 0: there is no snapshot.
    [[nodiscard]] Result<void> closeUndone(const JournalEntry& entry, const Json& beforeModules,
                                           const std::string& label, const std::vector<FailedModule>& notUndone,
                                           bool discarded);
    // An interrupted restore, finished (best effort, like any restore).
    [[nodiscard]] Result<void> finishRestore(const JournalEntry& entry, const std::string& label,
                                             bool hasSnapshot);
    // `fields`: what the operation is about; the fields we set before are always added.
    [[nodiscard]] Result<CaptureResult> captureInternalized(const IModuleLogic& module,
                                                            std::span<const std::string> fields = {});
    // Fields set for the first time keep their value from before in the base snapshot, so the
    // original Windows comes back for them too (a monitor connected later, a tweak).
    [[nodiscard]] Result<void> rememberOriginals(const Json& beforeModules, const Json& desiredAll);
    [[nodiscard]] Result<void> undoOpenJournalIfAny();
    void backUpOriginal();
    void collectGarbage(OperationReport& report);

    StateDb& m_state;
    ContentStore& m_store;
    const ModuleCatalog& m_catalog;
    IModuleRunner& m_runner;
    std::function<void(std::string_view)> m_afterModule;
    std::filesystem::path m_originalBackup;
    std::function<Json()> m_coreState;
};

} // namespace rmw::engine
