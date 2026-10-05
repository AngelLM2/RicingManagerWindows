#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/engine/ContentStore.hpp"
#include "rmw/engine/ModuleCatalog.hpp"
#include "rmw/engine/StateDb.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
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

struct OperationReport
{
    std::int64_t historyId = 0;       // 0 when nothing changed
    std::vector<std::string> modules; // modules that were changed
    std::vector<SkippedField> skipped;
    std::vector<std::string> warnings;
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
//      the failed one included, in reverse order
//   5. record lastApplied per field and the history entry; close the journal
//
// A crash between 3 and 5 leaves the journal open; recover() undoes it on the next start.
class RiceEngine
{
public:
    static constexpr std::size_t kHistoryLimit = 20;

    RiceEngine(StateDb& state, ContentStore& store, const ModuleCatalog& catalog, IModuleRunner& runner);

    // Undoes an operation interrupted by a crash. True when there was one to undo.
    // If the undo fails too, the "needs_restore" flag stays set and this retries next time.
    [[nodiscard]] Result<bool> recover();

    // Captures "my original Windows" once (first run). True when it was captured now.
    [[nodiscard]] Result<bool> ensureBaseSnapshot();

    [[nodiscard]] Result<OperationReport> apply(const domain::Rice& rice,
                                                const std::filesystem::path& packageRoot);

    // Puts back the original value of every field we changed, except fields the user changed
    // after us (listed in `skipped`) unless `force`.
    [[nodiscard]] Result<OperationReport> restoreOriginal(const RestoreOptions& options = {});

    // Returns to how things were right before history entry `historyId`, same field rules.
    [[nodiscard]] Result<OperationReport> restoreHistory(std::int64_t historyId,
                                                         const RestoreOptions& options = {});

    [[nodiscard]] Result<bool> needsRestore();

    // Like apply(), but no history entry: automatic changes (schedule, rules) are not something
    // the user would go back to (docs/03 §5). Modules Windows already shows as desired are left
    // alone, so a schedule check after resume does not re-apply the theme.
    [[nodiscard]] Result<OperationReport> applyAutomatic(const domain::Rice& rice,
                                                         const std::filesystem::path& packageRoot,
                                                         const std::string& label);

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
    [[nodiscard]] Result<std::vector<Step>> stepsFromRice(const domain::Rice& rice,
                                                          const std::filesystem::path& packageRoot);
    [[nodiscard]] Result<OperationReport> restoreTo(const Json& targetModules, const std::string& label,
                                                    const RestoreOptions& options, AppliedUpdate update);
    [[nodiscard]] Result<void> undo(const Json& beforeModules, const Json& desired,
                                    const std::vector<std::string>& touched);
    // `fields`: what the operation is about; the fields we set before are always added.
    [[nodiscard]] Result<CaptureResult> captureInternalized(const IModuleLogic& module,
                                                            std::span<const std::string> fields = {});
    // Fields set for the first time keep their value from before in the base snapshot, so the
    // original Windows comes back for them too (a monitor connected later, a tweak).
    [[nodiscard]] Result<void> rememberOriginals(const Json& beforeModules, const Json& desiredAll);
    [[nodiscard]] Result<void> undoOpenJournalIfAny();
    void collectGarbage(OperationReport& report);

    StateDb& m_state;
    ContentStore& m_store;
    const ModuleCatalog& m_catalog;
    IModuleRunner& m_runner;
    std::function<void(std::string_view)> m_afterModule;
};

} // namespace rmw::engine
