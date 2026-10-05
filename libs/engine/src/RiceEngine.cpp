#include "rmw/engine/RiceEngine.hpp"

#include "rmw/base/Log.hpp"

#include <algorithm>
#include <format>
#include <ranges>

namespace rmw::engine
{
namespace
{

constexpr std::string_view kNeedsRestore = "needs_restore";

std::string_view fidelityName(Fidelity fidelity) noexcept
{
    return fidelity == Fidelity::Exact ? "exact" : "lossy";
}

Json snapshotEntry(const CaptureResult& capture)
{
    return Json{{"state", capture.state},
                {"fidelity", fidelityName(capture.fidelity)},
                {"reason", capture.lossyReason}};
}

// The state of `module` inside a snapshot's modules document, or null when absent.
const Json* stateIn(const Json& modules, std::string_view module)
{
    const auto entry = modules.find(module);
    if (entry == modules.end() || !entry->is_object())
    {
        return nullptr;
    }
    const auto state = entry->find("state");
    return state != entry->end() && state->is_object() ? &*state : nullptr;
}

template <class T>
std::unexpected<Error> propagate(Result<T>& result, std::string_view context = {})
{
    if (context.empty())
    {
        return std::unexpected(std::move(result.error()));
    }
    return std::unexpected(std::move(result.error()).withContext(context));
}

} // namespace

RiceEngine::RiceEngine(StateDb& state, ContentStore& store, const ModuleCatalog& catalog,
                       IModuleRunner& runner)
    : m_state(state)
    , m_store(store)
    , m_catalog(catalog)
    , m_runner(runner)
{
}

Result<CaptureResult> RiceEngine::captureInternalized(const IModuleLogic& module)
{
    auto captured = m_runner.capture(module.id());
    if (!captured)
    {
        return propagate(captured, std::format("capture {}", module.id()));
    }
    if (!captured->state.is_object())
    {
        return fail(ErrorCode::InvalidData,
                    std::format("module {} captured a non-object state", module.id()));
    }
    StoreAssetSink sink(m_store, {});
    if (auto internalized = module.internalize(captured->state, sink); !internalized)
    {
        return propagate(internalized, std::format("store files of {}", module.id()));
    }
    return captured;
}

Result<bool> RiceEngine::needsRestore()
{
    auto flag = m_state.meta(kNeedsRestore);
    if (!flag)
    {
        return propagate(flag);
    }
    return flag->has_value();
}

// ---- recovery and the original snapshot -----------------------------------------------------------

Result<void> RiceEngine::undo(const Json& beforeModules, const Json& desired,
                              const std::vector<std::string>& touched)
{
    std::optional<Error> firstError;
    for (const auto& id : touched | std::views::reverse)
    {
        const Json* before = stateIn(beforeModules, id);
        const auto fields = desired.find(id);
        if (before == nullptr || fields == desired.end() || !fields->is_object())
        {
            continue;
        }
        // Only the fields this operation was changing; the rest were never touched.
        Json restore = Json::object();
        for (const auto& [field, value] : fields->items())
        {
            if (const auto original = before->find(field); original != before->end())
            {
                restore[field] = *original;
            }
        }
        if (restore.empty())
        {
            continue;
        }
        if (auto applied = m_runner.apply(id, restore); !applied)
        {
            log::error("undo {} failed: {}", id, applied.error().toString());
            if (!firstError)
            {
                firstError = std::move(applied.error()).withContext(std::format("undo {}", id));
            }
        }
    }
    if (firstError)
    {
        return std::unexpected(std::move(*firstError));
    }
    return {};
}

Result<void> RiceEngine::undoOpenJournalIfAny()
{
    auto recovered = recover();
    if (!recovered)
    {
        return propagate(recovered, "an earlier operation could not be undone");
    }
    return {};
}

Result<bool> RiceEngine::recover()
{
    auto journal = m_state.openJournal();
    if (!journal)
    {
        return propagate(journal);
    }
    if (!*journal)
    {
        return false;
    }
    const JournalEntry& entry = **journal;
    log::warn("undoing interrupted operation '{}' from {}", entry.operation, entry.startedAt);

    auto snapshot = m_state.snapshot(entry.snapshotId);
    if (!snapshot)
    {
        return propagate(snapshot);
    }
    if (!*snapshot)
    {
        return fail(ErrorCode::InvalidData, "the interrupted operation has no snapshot to undo with");
    }

    // Everything the operation meant to change, in apply order (undo reverses it).
    std::vector<std::string> touched;
    for (const auto* module : m_catalog.ordered())
    {
        if (entry.desired.contains(module->id()))
        {
            touched.emplace_back(module->id());
        }
    }

    if (auto undone = undo((*snapshot)->modules, entry.desired, touched); !undone)
    {
        if (auto flagged = m_state.setMeta(kNeedsRestore, "1"); !flagged)
        {
            log::error("cannot record needs_restore: {}", flagged.error().toString());
        }
        return propagate(undone, "undo the interrupted operation");
    }

    auto transaction = m_state.transaction();
    if (!transaction)
    {
        return propagate(transaction);
    }
    for (auto step : {m_state.closeJournal(), m_state.deleteSnapshot(entry.snapshotId),
                      m_state.setMeta(kNeedsRestore, std::nullopt)})
    {
        if (!step)
        {
            return propagate(step);
        }
    }
    if (auto committed = transaction->commit(); !committed)
    {
        return propagate(committed);
    }
    return true;
}

Result<bool> RiceEngine::ensureBaseSnapshot()
{
    auto base = m_state.baseSnapshot();
    if (!base)
    {
        return propagate(base);
    }
    if (*base)
    {
        return false;
    }

    Json modules = Json::object();
    for (const auto* module : m_catalog.ordered())
    {
        auto captured = captureInternalized(*module);
        if (!captured)
        {
            // A missing module only means "cannot restore that one"; do not block first run.
            log::error("original snapshot: {}", captured.error().toString());
            continue;
        }
        modules[std::string(module->id())] = snapshotEntry(*captured);
    }
    if (auto added = m_state.addSnapshot(SnapshotKind::Base, "Original Windows", modules); !added)
    {
        return propagate(added);
    }
    return true;
}

// ---- operations -----------------------------------------------------------------------------

Result<OperationReport> RiceEngine::apply(const domain::Rice& rice, const std::filesystem::path& packageRoot)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }
    if (auto base = ensureBaseSnapshot(); !base)
    {
        return propagate(base);
    }

    StoreAssetSink assets(m_store, packageRoot);
    std::vector<Step> steps;
    for (const auto* module : m_catalog.ordered())
    {
        auto desired = module->desiredFromRice(rice, assets);
        if (!desired)
        {
            return propagate(desired, std::format("rice section for {}", module->id()));
        }
        if (!desired->is_object())
        {
            return fail(ErrorCode::InvalidData,
                        std::format("module {} produced a non-object configuration", module->id()));
        }
        if (!desired->empty())
        {
            steps.push_back({module, std::move(*desired), std::nullopt});
        }
    }
    if (steps.empty())
    {
        return fail(ErrorCode::InvalidArgument, "this rice does not change anything");
    }
    return run("apply", std::format("Applied \"{}\"", rice.meta.name), std::move(steps), AppliedUpdate::Merge,
               {});
}

Result<OperationReport> RiceEngine::restoreOriginal(const RestoreOptions& options)
{
    auto base = m_state.baseSnapshot();
    if (!base)
    {
        return propagate(base);
    }
    if (!*base)
    {
        return fail(ErrorCode::NotFound, "there is no original snapshot yet (nothing was ever applied)");
    }
    return restoreTo((*base)->modules, "Restored the original Windows", options, AppliedUpdate::Remove);
}

Result<OperationReport> RiceEngine::restoreHistory(std::int64_t historyId, const RestoreOptions& options)
{
    auto entry = m_state.historyEntry(historyId);
    if (!entry)
    {
        return propagate(entry);
    }
    if (!*entry)
    {
        return fail(ErrorCode::NotFound, std::format("history entry {} does not exist", historyId));
    }
    auto snapshot = m_state.snapshot((*entry)->snapshotId);
    if (!snapshot)
    {
        return propagate(snapshot);
    }
    if (!*snapshot)
    {
        return fail(ErrorCode::NotFound, std::format("history entry {} has no snapshot", historyId));
    }
    return restoreTo((*snapshot)->modules, std::format("Went back to before \"{}\"", (*entry)->label),
                     options, AppliedUpdate::Merge);
}

Result<OperationReport> RiceEngine::restoreTo(const Json& targetModules, const std::string& label,
                                              const RestoreOptions& options, AppliedUpdate update)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }

    OperationReport report;
    std::vector<Step> steps;
    for (const auto* module : m_catalog.ordered())
    {
        const std::string id(module->id());
        auto applied = m_state.applied(id);
        if (!applied)
        {
            return propagate(applied);
        }
        if (applied->empty())
        {
            continue; // we never changed this module
        }
        const Json* target = stateIn(targetModules, id);

        auto current = captureInternalized(*module);
        if (!current)
        {
            return propagate(current);
        }

        // Three-way, per field: original (target) / what we set (applied) / what is there now.
        Json desired = Json::object();
        for (const auto& [field, ours] : applied->items())
        {
            if (target == nullptr || !target->contains(field))
            {
                report.skipped.push_back({id, field, "not in the snapshot"});
                continue;
            }
            const auto now = current->state.find(field);
            if (!options.force && now != current->state.end() && !module->fieldEquivalent(field, *now, ours))
            {
                report.skipped.push_back({id, field, "changed after it was applied"});
                continue;
            }
            desired[field] = (*target)[field];
        }
        if (!desired.empty())
        {
            steps.push_back({module, std::move(desired), std::move(*current)});
        }
    }
    if (steps.empty())
    {
        return report; // nothing to do
    }
    return run("restore", label, std::move(steps), update, std::move(report));
}

Result<OperationReport> RiceEngine::run(std::string_view operation, const std::string& label,
                                        std::vector<Step> steps, AppliedUpdate update, OperationReport report)
{
    // 2. current state of every module involved
    Json beforeModules = Json::object();
    Json desiredAll = Json::object();
    for (auto& step : steps)
    {
        const std::string id(step.module->id());
        if (!step.current)
        {
            auto captured = captureInternalized(*step.module);
            if (!captured)
            {
                return propagate(captured);
            }
            step.current = std::move(*captured);
        }
        if (step.current->fidelity == Fidelity::Lossy)
        {
            report.warnings.push_back(std::format("{}: {}", id, step.current->lossyReason));
        }
        beforeModules[id] = snapshotEntry(*step.current);
        desiredAll[id] = step.desired;
    }

    // 3. durable "in progress" marker
    std::int64_t snapshotId = 0;
    {
        auto transaction = m_state.transaction();
        if (!transaction)
        {
            return propagate(transaction);
        }
        auto added = m_state.addSnapshot(SnapshotKind::Before, label, beforeModules);
        if (!added)
        {
            return propagate(added);
        }
        snapshotId = *added;
        if (auto journaled = m_state.beginJournal(operation, snapshotId, desiredAll); !journaled)
        {
            return propagate(journaled);
        }
        if (auto committed = transaction->commit(); !committed)
        {
            return propagate(committed);
        }
    }

    // 4. apply in dependency order; undo everything touched on the first failure
    std::vector<std::string> touched;
    for (const auto& step : steps)
    {
        const std::string id(step.module->id());
        touched.push_back(id);
        auto applied = m_runner.apply(id, step.desired);
        if (applied)
        {
            if (m_afterModule)
            {
                m_afterModule(id);
            }
            continue;
        }
        Error error = std::move(applied.error()).withContext(std::format("apply {}", id));
        log::error("{} failed: {}; undoing", operation, error.toString());
        if (auto undone = undo(beforeModules, desiredAll, touched); !undone)
        {
            if (auto flagged = m_state.setMeta(kNeedsRestore, "1"); !flagged)
            {
                log::error("cannot record needs_restore: {}", flagged.error().toString());
            }
            return std::unexpected(std::move(error).withContext(std::format(
                "undo also failed ({}); it will be retried on the next start", undone.error().message())));
        }
        auto transaction = m_state.transaction();
        if (transaction)
        {
            const bool closed = m_state.closeJournal() && m_state.deleteSnapshot(snapshotId);
            if (!closed || !transaction->commit())
            {
                log::error("cannot close the journal after undo; it will be undone again on the next start");
            }
        }
        return std::unexpected(std::move(error));
    }

    // 5. lastApplied per field, history, journal closed — in one database transaction
    std::vector<std::pair<std::string, Json>> appliedUpdates;
    for (const auto& step : steps)
    {
        const std::string id(step.module->id());
        auto existing = m_state.applied(id);
        if (!existing)
        {
            return propagate(existing);
        }
        if (update == AppliedUpdate::Remove)
        {
            for (const auto& [field, value] : step.desired.items())
            {
                existing->erase(field);
            }
        }
        else
        {
            // Record what Windows actually holds now (it may normalize what we wrote).
            // Same shape as every other capture (markers such as "keep" resolved), so later
            // three-way comparisons compare like with like.
            auto after = captureInternalized(*step.module);
            for (const auto& [field, value] : step.desired.items())
            {
                const bool captured = after && after->state.is_object() && after->state.contains(field);
                (*existing)[field] = captured ? after->state[field] : value;
            }
        }
        appliedUpdates.emplace_back(id, std::move(*existing));
        report.modules.push_back(id);
    }

    auto transaction = m_state.transaction();
    if (!transaction)
    {
        return propagate(transaction);
    }
    for (const auto& [id, fields] : appliedUpdates)
    {
        if (auto saved = m_state.setApplied(id, fields); !saved)
        {
            return propagate(saved);
        }
    }
    auto historyId = m_state.addHistory(snapshotId, label, kHistoryLimit);
    if (!historyId)
    {
        return propagate(historyId);
    }
    if (auto closed = m_state.closeJournal(); !closed)
    {
        return propagate(closed);
    }
    if (auto committed = transaction->commit(); !committed)
    {
        return propagate(committed);
    }
    report.historyId = *historyId;
    collectGarbage(report);
    return report;
}

void RiceEngine::collectGarbage(OperationReport& report)
{
    bool queryFailed = false;
    auto removed = m_store.collectGarbage([&](std::string_view sha256) {
        auto referenced = m_state.referencesAsset(sha256);
        if (!referenced)
        {
            queryFailed = true;
            return true; // when in doubt, keep
        }
        return *referenced;
    });
    if (!removed || queryFailed)
    {
        report.warnings.emplace_back("could not clean up unused files in the content store");
    }
}

} // namespace rmw::engine
