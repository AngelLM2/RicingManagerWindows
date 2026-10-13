#include "rmw/engine/RiceEngine.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <algorithm>
#include <format>
#include <ranges>

namespace rmw::engine
{
namespace
{

constexpr std::string_view kNeedsRestore = "needs_restore";
// A crash in the middle of a restore is finished on the next start, never undone (undoing it would
// apply the rice again); the journal says which restore it was.
constexpr std::string_view kRestoreOriginal = "restore";        // the fields are no longer ours after it
constexpr std::string_view kRestoreHistory = "restore.history"; // the fields stay ours
constexpr std::uint64_t kBackupMaxBytes = std::uint64_t{64} << 20;

bool isRestore(std::string_view operation) noexcept
{
    return operation == kRestoreOriginal || operation == kRestoreHistory;
}

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

// A rice no module has anything of: fine when it changes something else (a bar), else a mistake.
Result<OperationReport> nothingToChange(bool changesElsewhere)
{
    if (changesElsewhere)
    {
        return OperationReport{};
    }
    return fail(ErrorCode::InvalidArgument, "this rice does not change anything");
}

std::string moduleNames(const std::vector<FailedModule>& modules)
{
    std::string names;
    for (const auto& module : modules)
    {
        names += (names.empty() ? "" : ", ") + module.module;
    }
    return names;
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

Result<CaptureResult> RiceEngine::captureInternalized(const IModuleLogic& module,
                                                      std::span<const std::string> asked)
{
    std::vector<std::string> fields(asked.begin(), asked.end());
    if (auto applied = m_state.applied(module.id()); applied && applied->is_object())
    {
        for (const auto& [field, value] : applied->items())
        {
            if (std::ranges::find(fields, field) == fields.end())
            {
                fields.push_back(field);
            }
        }
    }
    auto captured = m_runner.capture(module.id(), fields);
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

std::vector<FailedModule> RiceEngine::undo(const Json& beforeModules, const Json& desired,
                                           const std::vector<std::string>& touched)
{
    std::vector<FailedModule> failed;
    for (const auto& id : touched | std::views::reverse)
    {
        const auto fields = desired.find(id);
        if (fields == desired.end() || !fields->is_object() || fields->empty())
        {
            continue;
        }
        const Json* before = stateIn(beforeModules, id);
        if (before == nullptr)
        {
            failed.push_back({id, "how it was before is not known"});
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
            failed.push_back({id, applied.error().message()});
        }
    }
    return failed;
}

Result<void> RiceEngine::closeUndone(const JournalEntry& entry, const Json& beforeModules, const std::string& label,
                                     const std::vector<FailedModule>& notUndone, bool discarded)
{
    // What was not undone stays as the operation left it: ours from now on, so a restore puts it back
    // (and the base snapshot learns the originals of the fields it set for the first time).
    Json kept = Json::object();
    for (const auto& failed : notUndone)
    {
        if (const auto fields = entry.desired.find(failed.module);
            fields != entry.desired.end() && fields->is_object() && !fields->empty())
        {
            kept[failed.module] = *fields;
        }
    }
    if (!kept.empty())
    {
        if (auto remembered = rememberOriginals(beforeModules, kept); !remembered)
        {
            log::error("cannot remember the originals of what was not undone: {}", remembered.error().toString());
        }
    }
    std::vector<std::pair<std::string, Json>> adopted;
    for (const auto& [id, fields] : kept.items())
    {
        auto existing = m_state.applied(id);
        if (!existing)
        {
            return propagate(existing);
        }
        std::optional<CaptureResult> now;
        // A discarded operation runs no module code at all (safe mode: that may be what crashes).
        if (const IModuleLogic* module = m_catalog.find(id); module != nullptr && !discarded)
        {
            std::vector<std::string> names;
            for (const auto& [field, value] : fields.items())
            {
                names.push_back(field);
            }
            if (auto captured = captureInternalized(*module, names))
            {
                now = std::move(*captured);
            }
        }
        for (const auto& [field, value] : fields.items())
        {
            // What Windows shows now when it can be read, else what the operation was writing.
            const bool shown = now && now->state.contains(field);
            (*existing)[field] = shown ? now->state[field] : value;
        }
        adopted.emplace_back(id, std::move(*existing));
    }

    auto transaction = m_state.transaction();
    if (!transaction)
    {
        return propagate(transaction);
    }
    for (const auto& [id, fields] : adopted)
    {
        if (auto saved = m_state.setApplied(id, fields); !saved)
        {
            return propagate(saved);
        }
    }
    if (auto closed = m_state.closeJournal(); !closed)
    {
        return propagate(closed);
    }
    if (notUndone.empty())
    {
        if (auto dropped = m_state.deleteSnapshot(entry.snapshotId); !dropped)
        {
            return propagate(dropped);
        }
    }
    else if (entry.snapshotId != 0)
    {
        // How things were right before it: going back one step stays possible.
        const std::string history =
            std::format("{} ({})", label, discarded ? "not undone" : "not fully undone");
        if (auto added = m_state.addHistory(entry.snapshotId, history, kHistoryLimit); !added)
        {
            return propagate(added);
        }
    }
    const bool flag = !discarded && !notUndone.empty();
    if (discarded || flag)
    {
        if (auto flagged = m_state.setMeta(kNeedsRestore, flag ? std::optional<std::string_view>("1") : std::nullopt);
            !flagged)
        {
            return propagate(flagged);
        }
    }
    return transaction->commit();
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
        // A journal that cannot be read cannot be undone either; kept, it would block every operation.
        log::error("the interrupted operation cannot be read ({}); it is dropped", journal.error().toString());
        auto transaction = m_state.transaction();
        if (!transaction)
        {
            return propagate(transaction);
        }
        for (auto step : {m_state.closeJournal(), m_state.setMeta(kNeedsRestore, "1")})
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
    if (!*journal)
    {
        return false;
    }
    const JournalEntry entry = std::move(**journal);
    auto snapshot = m_state.snapshot(entry.snapshotId);
    if (!snapshot)
    {
        return propagate(snapshot);
    }
    const bool hasSnapshot = snapshot->has_value();
    const std::string label = hasSnapshot ? (*snapshot)->label : entry.operation;

    if (isRestore(entry.operation))
    {
        log::warn("finishing the interrupted '{}' from {}", label, entry.startedAt);
        if (auto finished = finishRestore(entry, label, hasSnapshot); !finished)
        {
            return propagate(finished, "finish the interrupted restore");
        }
        return true;
    }

    log::warn("undoing the interrupted '{}' ({}) from {}", label, entry.operation, entry.startedAt);
    // Everything the operation meant to change, in apply order (undo reverses it).
    std::vector<std::string> touched;
    for (const auto* module : m_catalog.ordered())
    {
        if (entry.desired.contains(module->id()))
        {
            touched.emplace_back(module->id());
        }
    }
    const Json beforeModules = hasSnapshot ? (*snapshot)->modules : Json::object();
    if (!hasSnapshot)
    {
        log::error("the interrupted operation has no snapshot to undo with: what it changed stays");
    }
    const auto notUndone = undo(beforeModules, entry.desired, touched);
    if (!notUndone.empty())
    {
        log::error("not undone: {}; restoring the original puts it back", moduleNames(notUndone));
    }
    JournalEntry closing = entry;
    closing.snapshotId = hasSnapshot ? entry.snapshotId : 0;
    if (auto closed = closeUndone(closing, beforeModules, label, notUndone, false); !closed)
    {
        return propagate(closed, "close the interrupted operation");
    }
    return true;
}

Result<bool> RiceEngine::discardJournal()
{
    auto journal = m_state.openJournal();
    std::optional<JournalEntry> entry;
    if (journal && *journal)
    {
        entry = std::move(**journal);
    }
    if (!journal || !entry)
    {
        // Nothing (readable) to keep: the row goes, and so does the flag.
        auto transaction = m_state.transaction();
        if (!transaction)
        {
            return propagate(transaction);
        }
        for (auto step : {m_state.closeJournal(), m_state.setMeta(kNeedsRestore, std::nullopt)})
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
        return !journal; // an unreadable one was there
    }
    auto snapshot = m_state.snapshot(entry->snapshotId);
    if (!snapshot)
    {
        return propagate(snapshot);
    }
    const bool hasSnapshot = snapshot->has_value();
    const std::string label = hasSnapshot ? (*snapshot)->label : entry->operation;
    log::warn("forgetting the interrupted '{}' without undoing it", label);
    std::vector<FailedModule> kept;
    for (const auto& [id, fields] : entry->desired.items())
    {
        kept.push_back({id, "discarded"});
    }
    JournalEntry closing = *entry;
    closing.snapshotId = hasSnapshot ? entry->snapshotId : 0;
    if (auto closed = closeUndone(closing, hasSnapshot ? (*snapshot)->modules : Json::object(), label, kept, true);
        !closed)
    {
        return propagate(closed);
    }
    return true;
}

Result<void> RiceEngine::finishRestore(const JournalEntry& entry, const std::string& label, bool hasSnapshot)
{
    std::vector<Step> steps;
    for (const auto* module : m_catalog.ordered())
    {
        const auto fields = entry.desired.find(module->id());
        if (fields != entry.desired.end() && fields->is_object() && !fields->empty())
        {
            steps.push_back({module, *fields, std::nullopt});
        }
    }
    OperationReport report;
    for (const auto& step : steps)
    {
        const std::string id(step.module->id());
        if (auto applied = m_runner.apply(id, step.desired); !applied)
        {
            log::warn("restore: {} stays as it is: {}", id, applied.error().toString());
            report.failed.push_back({id, applied.error().message()});
        }
    }
    if (report.failed.size() == steps.size())
    {
        // Nothing came back: nothing to record (what is ours stays ours).
        auto transaction = m_state.transaction();
        if (!transaction)
        {
            return propagate(transaction);
        }
        for (auto step : {m_state.closeJournal(), m_state.deleteSnapshot(entry.snapshotId)})
        {
            if (!step)
            {
                return propagate(step);
            }
        }
        return transaction->commit();
    }
    const AppliedUpdate update = entry.operation == kRestoreOriginal ? AppliedUpdate::Remove : AppliedUpdate::Merge;
    // Without its snapshot there is no history entry to make (nor anything to go back to).
    auto recorded = recordOperation(label, steps, update, entry.snapshotId, !hasSnapshot, report);
    if (!recorded)
    {
        return propagate(recorded);
    }
    backUpOriginal(); // what we hold changed
    return {};
}

Result<std::optional<SnapshotRecord>> RiceEngine::originalSnapshot()
{
    auto base = m_state.baseSnapshot();
    if (!base || *base || m_originalBackup.empty())
    {
        return base;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_originalBackup, ec))
    {
        return base; // nothing was ever applied
    }
    auto bytes = platform::readFileLimited(m_originalBackup, kBackupMaxBytes);
    if (!bytes)
    {
        log::error("the copy of the original snapshot cannot be read: {}", bytes.error().toString());
        return base;
    }
    json::ParseLimits limits;
    limits.maxBytes = kBackupMaxBytes;
    limits.maxElements = 1'000'000;
    auto saved = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), limits);
    if (!saved || !saved->is_object() || !json::memberOr(*saved, "modules", Json()).is_object())
    {
        log::error("the copy of the original snapshot is damaged; it is not used");
        return base;
    }
    SnapshotRecord record;
    record.kind = SnapshotKind::Base;
    record.label = json::stringOr(*saved, "label", "Original Windows");
    record.createdAt = json::intOr(*saved, "createdAt", 0);
    record.modules = (*saved)["modules"];
    log::warn("state.db has no original snapshot: the copy kept outside it (taken at {}) comes back",
              record.createdAt);
    if (auto added = m_state.addSnapshot(record); !added)
    {
        return propagate(added);
    }
    // What we held then is ours again (the data folder lost with a rice still on, the app installed again):
    // its fields are not taken as the person's own, and a restore brings their original back over them.
    if (const auto applied = saved->find("applied"); applied != saved->end() && applied->is_object())
    {
        std::size_t held = 0;
        for (const auto& [id, fields] : applied->items())
        {
            auto existing = m_state.applied(id);
            if (!existing || !existing->empty() || !fields.is_object() || fields.empty())
            {
                continue;
            }
            if (auto set = m_state.setApplied(id, fields); !set)
            {
                log::warn("{} held before cannot be ours again: {}", id, set.error().toString());
                continue;
            }
            ++held;
        }
        if (held > 0)
        {
            log::warn("what the app held before ({} module(s)) is ours again: a restore brings the original back",
                      held);
        }
    }
    return m_state.baseSnapshot();
}

void RiceEngine::backUpOriginal()
{
    if (m_originalBackup.empty())
    {
        return;
    }
    auto base = m_state.baseSnapshot();
    if (!base || !*base)
    {
        return;
    }
    // With what we hold now: a state.db lost with a rice still on knows which fields are not the person's.
    Json applied = Json::object();
    if (auto modules = m_state.appliedModules())
    {
        for (const auto& id : *modules)
        {
            if (auto fields = m_state.applied(id); fields && !fields->empty())
            {
                applied[id] = std::move(*fields);
            }
        }
    }
    const std::string text = json::dump(Json{{"label", (*base)->label},
                                             {"createdAt", (*base)->createdAt},
                                             {"modules", (*base)->modules},
                                             {"applied", std::move(applied)}});
    if (auto written = platform::writeFileAtomic(m_originalBackup, std::as_bytes(std::span(text))); !written)
    {
        log::warn("cannot keep a copy of the original snapshot: {}", written.error().toString());
    }
}

Result<bool> RiceEngine::ensureBaseSnapshot()
{
    auto base = originalSnapshot();
    if (!base)
    {
        return propagate(base);
    }
    if (*base)
    {
        std::error_code ec;
        if (!m_originalBackup.empty() && !std::filesystem::exists(m_originalBackup, ec))
        {
            backUpOriginal(); // taken before there was a copy
        }
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
    backUpOriginal();
    return true;
}

// ---- operations -----------------------------------------------------------------------------

Result<OperationReport> RiceEngine::apply(const domain::Rice& rice, const std::filesystem::path& packageRoot,
                                          bool changesElsewhere)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }
    if (auto base = ensureBaseSnapshot(); !base)
    {
        return propagate(base);
    }

    auto steps = stepsFromRice(rice, packageRoot);
    if (!steps)
    {
        return propagate(steps);
    }
    if (steps->empty())
    {
        return nothingToChange(changesElsewhere);
    }
    return run("apply", std::format("Applied \"{}\"", rice.meta.name), std::move(*steps),
               AppliedUpdate::Merge, {});
}

Result<std::vector<RiceEngine::Step>> RiceEngine::stepsFromRice(const domain::Rice& rice,
                                                                const std::filesystem::path& packageRoot)
{
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
    return steps;
}

Result<OperationReport> RiceEngine::applyAutomatic(const domain::Rice& rice,
                                                   const std::filesystem::path& packageRoot,
                                                   const std::string& label, bool changesElsewhere)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }
    // Even an automatic change may be the first one: the original must be known before it.
    if (auto base = ensureBaseSnapshot(); !base)
    {
        return propagate(base);
    }
    auto steps = stepsFromRice(rice, packageRoot);
    if (!steps)
    {
        return propagate(steps);
    }
    if (steps->empty())
    {
        return nothingToChange(changesElsewhere);
    }
    return run("automatic", label, std::move(*steps), AppliedUpdate::Merge, {}, true);
}

Result<OperationReport> RiceEngine::applyFields(std::string_view moduleId, const Json& fields,
                                                const std::string& label)
{
    const IModuleLogic* module = m_catalog.find(moduleId);
    if (module == nullptr)
    {
        return fail(ErrorCode::NotFound, std::format("unknown module '{}'", moduleId));
    }
    if (!fields.is_object() || fields.empty())
    {
        return fail(ErrorCode::InvalidArgument, "no fields to apply");
    }
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }
    if (auto base = ensureBaseSnapshot(); !base)
    {
        return propagate(base);
    }
    std::vector<Step> steps;
    steps.push_back({module, fields, std::nullopt});
    return run("automatic", label, std::move(steps), AppliedUpdate::Merge, {}, true);
}

Result<std::vector<std::string>> RiceEngine::driftedFields(std::string_view moduleId)
{
    const IModuleLogic* module = m_catalog.find(moduleId);
    if (module == nullptr)
    {
        return fail(ErrorCode::NotFound, std::format("unknown module '{}'", moduleId));
    }
    auto applied = m_state.applied(moduleId);
    if (!applied)
    {
        return propagate(applied);
    }
    std::vector<std::string> drifted;
    if (applied->empty())
    {
        return drifted;
    }
    auto current = captureInternalized(*module);
    if (!current)
    {
        return propagate(current);
    }
    for (const auto& [field, ours] : applied->items())
    {
        const auto now = current->state.find(field);
        if (now != current->state.end() && !module->fieldEquivalent(field, *now, ours))
        {
            drifted.push_back(field);
        }
    }
    return drifted;
}

Result<void> RiceEngine::detach(std::string_view moduleId, const std::vector<std::string>& fields)
{
    auto applied = m_state.applied(moduleId);
    if (!applied)
    {
        return propagate(applied);
    }
    for (const auto& field : fields)
    {
        applied->erase(field);
    }
    if (auto set = m_state.setApplied(moduleId, *applied); !set)
    {
        return set;
    }
    backUpOriginal(); // the person took them: theirs, not ours, if the data folder is ever lost
    return {};
}

Result<OperationReport> RiceEngine::reapplyManaged(std::span<const std::string> modules,
                                                   std::span<const std::string> windowsResets)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        return propagate(clean);
    }
    std::vector<Step> steps;
    for (const auto* module : m_catalog.ordered())
    {
        const std::string id(module->id());
        if (!modules.empty() && std::ranges::find(modules, id) == modules.end())
        {
            continue;
        }
        // Where the event that asks for this does not reset Windows, what differs from what we set is the
        // person's (a sound, an icon, a tweak they changed by hand): theirs now, not set again. v1.8 put it
        // back at every Explorer restart.
        if (std::ranges::find(windowsResets, id) == windowsResets.end())
        {
            if (auto drifted = driftedFields(id); drifted && !drifted->empty())
            {
                std::string names;
                for (const auto& field : *drifted)
                {
                    names += (names.empty() ? "" : ", ") + field;
                }
                log::info("reapply: {} changed outside since we set it ({}): left as it is now", id, names);
                if (auto released = detach(id, *drifted); !released)
                {
                    return propagate(released);
                }
            }
        }
        auto applied = m_state.applied(id);
        if (!applied)
        {
            return propagate(applied);
        }
        if (!applied->empty())
        {
            steps.push_back({module, std::move(*applied), std::nullopt});
        }
    }
    if (steps.empty())
    {
        return OperationReport{};
    }
    return run("reapply", "Reapplied after a system change", std::move(steps), AppliedUpdate::Merge, {},
               true);
}

Result<OperationReport> RiceEngine::restoreOriginal(const RestoreOptions& options)
{
    auto base = originalSnapshot();
    if (!base)
    {
        return propagate(base);
    }
    if (!*base)
    {
        return fail(ErrorCode::NotFound, "there is no original snapshot yet (nothing was ever applied)");
    }
    return restoreTo(**base, kRestoreOriginal, "Restored the original Windows", options, AppliedUpdate::Remove);
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
    auto report = restoreTo(**snapshot, kRestoreHistory, std::format("Went back to before \"{}\"", (*entry)->label),
                            options, AppliedUpdate::Merge);
    if (report)
    {
        report->core = (*snapshot)->core;
    }
    return report;
}

Result<OperationReport> RiceEngine::restoreTo(const SnapshotRecord& target, std::string_view operation,
                                              const std::string& label, const RestoreOptions& options,
                                              AppliedUpdate update)
{
    if (auto clean = undoOpenJournalIfAny(); !clean)
    {
        // Going back must stay possible whatever happened to the last operation.
        log::error("{}; it is dropped and the restore goes on", clean.error().toString());
        if (auto dropped = discardJournal(); !dropped)
        {
            return propagate(clean);
        }
    }

    OperationReport report;
    std::vector<Step> steps;
    std::optional<SnapshotRecord> base;
    if (update == AppliedUpdate::Merge)
    {
        if (auto found = m_state.baseSnapshot(); found && *found)
        {
            base = std::move(**found);
        }
    }
    for (const auto* module : m_catalog.ordered())
    {
        const std::string id(module->id());
        if (!options.modules.empty() && std::ranges::find(options.modules, id) == options.modules.end())
        {
            continue;
        }
        auto applied = m_state.applied(id);
        if (!applied)
        {
            return propagate(applied);
        }
        // Going back past an operation also brings back what it changed and we no longer hold (a
        // restore of the original took the rice's fields away from us).
        Json then = Json::object();
        if (const auto changed = target.changes.find(id); changed != target.changes.end() && changed->is_object())
        {
            then = *changed;
        }
        if (applied->empty() && then.empty())
        {
            continue; // we never changed this module
        }
        const Json* wanted = stateIn(target.modules, id);
        std::optional<Json> original;
        if (wanted != nullptr && update == AppliedUpdate::Remove)
        {
            original = *wanted;
            module->forOriginal(*original);
            wanted = &*original;
        }

        std::vector<std::string> fields;
        for (const auto& [field, value] : then.items())
        {
            fields.push_back(field);
        }
        auto current = captureInternalized(*module, fields);
        if (!current)
        {
            // Like a module that fails to apply: it stays as it is, and the others still come back.
            log::warn("{}: {} stays as it is: {}", operation, id, current.error().toString());
            report.failed.push_back({id, current.error().message()});
            continue;
        }

        // Going back in the history: a field that snapshot never took (a later operation took it first) was
        // the person's then, as the original keeps it. v1.8 left it as the later rice set it.
        const Json* fallback = update == AppliedUpdate::Merge && base ? stateIn(base->modules, id) : nullptr;

        // Three-way, per field: the target / what we set (applied, or what that operation set) / now.
        Json desired = Json::object();
        const auto consider = [&](const std::string& field, const Json& ours, bool managed) {
            const Json* from = wanted != nullptr && wanted->contains(field) ? wanted
                               : fallback != nullptr && fallback->contains(field) ? fallback
                                                                                 : nullptr;
            if (from == nullptr)
            {
                if (managed)
                {
                    report.skipped.push_back({id, field, "not in the snapshot"});
                }
                return;
            }
            const auto now = current->state.find(field);
            if (!managed && now != current->state.end() && module->fieldEquivalent(field, *now, (*from)[field]))
            {
                return; // already as it was then
            }
            if (!options.force && now != current->state.end() && !module->fieldEquivalent(field, *now, ours))
            {
                report.skipped.push_back({id, field, "changed after it was applied"});
                return;
            }
            desired[field] = (*from)[field];
        };
        for (const auto& [field, ours] : applied->items())
        {
            consider(field, ours, true);
        }
        for (const auto& [field, set] : then.items())
        {
            if (!applied->contains(field))
            {
                consider(field, set, false);
            }
        }
        if (!desired.empty())
        {
            steps.push_back({module, std::move(desired), std::move(*current)});
        }
    }
    if (steps.empty())
    {
        if (!report.failed.empty())
        {
            // Nothing came back.
            return fail(ErrorCode::System,
                        std::format("{}: {}", report.failed.front().module, report.failed.front().reason));
        }
        if (update == AppliedUpdate::Remove)
        {
            // Nothing of ours is left to put back.
            if (auto cleared = m_state.setMeta(kNeedsRestore, std::nullopt); !cleared)
            {
                log::warn("cannot clear needs_restore: {}", cleared.error().toString());
            }
        }
        return report; // nothing to do
    }
    return run(operation, label, std::move(steps), update, std::move(report));
}

Result<void> RiceEngine::rememberOriginals(const Json& beforeModules, const Json& desiredAll)
{
    auto base = m_state.baseSnapshot();
    if (!base)
    {
        return std::unexpected(std::move(base.error()));
    }
    if (!*base)
    {
        return {};
    }
    Json modules = (*base)->modules;
    bool changed = false;
    for (const auto& [id, fields] : desiredAll.items())
    {
        const Json* before = stateIn(beforeModules, id);
        auto applied = m_state.applied(id);
        if (before == nullptr || !fields.is_object() || !applied)
        {
            continue;
        }
        Json& entry = modules[id];
        if (!entry.is_object())
        {
            entry = Json{{"state", Json::object()}, {"fidelity", "exact"}, {"reason", ""}};
        }
        Json& state = entry["state"];
        if (!state.is_object())
        {
            state = Json::object();
        }
        for (const auto& [field, value] : fields.items())
        {
            // A field we hold now: "before" is our own value, not the original. One we do not hold is the
            // person's: from before our first apply, or what they chose since (after a restore, or a change
            // of theirs that took it from us). That is the original to come back to; the one taken weeks
            // ago brought back an older Windows.
            if (applied->contains(field))
            {
                continue;
            }
            if (const auto original = before->find(field);
                original != before->end() && (!state.contains(field) || state[field] != *original))
            {
                state[field] = *original;
                changed = true;
            }
        }
    }
    if (!changed)
    {
        return {};
    }
    if (auto saved = m_state.setSnapshotModules((*base)->id, modules); !saved)
    {
        return saved;
    }
    backUpOriginal();
    return {};
}

Result<OperationReport> RiceEngine::run(std::string_view operation, const std::string& label,
                                        std::vector<Step> steps, AppliedUpdate update, OperationReport report,
                                        bool automatic)
{
    // 2. current state of every module involved
    Json beforeModules = Json::object();
    Json desiredAll = Json::object();
    for (auto& step : steps)
    {
        const std::string id(step.module->id());
        if (!step.current)
        {
            std::vector<std::string> fields;
            for (const auto& [field, value] : step.desired.items())
            {
                fields.push_back(field);
            }
            auto captured = captureInternalized(*step.module, fields);
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
    if (automatic)
    {
        // Nothing to do where Windows already shows what we want.
        std::erase_if(steps, [&](const Step& step) {
            const Json& now = step.current->state;
            for (const auto& [field, value] : step.desired.items())
            {
                const auto found = now.find(field);
                if (found == now.end() || !step.module->fieldEquivalent(field, *found, value))
                {
                    return false;
                }
            }
            const std::string id(step.module->id());
            beforeModules.erase(id);
            desiredAll.erase(id);
            return true;
        });
        if (steps.empty())
        {
            return report;
        }
    }

    // Every operation that takes fields (rules and slideshow pages too), not one that gives them back.
    if (!isRestore(operation) && operation != "reapply")
    {
        if (auto remembered = rememberOriginals(beforeModules, desiredAll); !remembered)
        {
            return propagate(remembered);
        }
    }

    // 3. durable "in progress" marker; the snapshot also keeps what the operation changes and the
    // core's own state, for a history entry to bring back
    SnapshotRecord before;
    before.label = label;
    before.modules = beforeModules;
    before.changes = desiredAll;
    if (m_coreState && !automatic)
    {
        before.core = m_coreState();
    }
    std::int64_t snapshotId = 0;
    {
        auto transaction = m_state.transaction();
        if (!transaction)
        {
            return propagate(transaction);
        }
        auto added = m_state.addSnapshot(before);
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

    // 4. apply in dependency order; undo everything touched on the first failure (a restore goes on)
    const bool bestEffort = isRestore(operation);
    std::vector<std::string> touched;
    std::size_t applyFailures = 0;
    std::optional<Error> firstFailure;
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
        if (bestEffort)
        {
            log::warn("{}: {} stays as it is: {}", operation, id, error.toString());
            report.failed.push_back({id, error.message()});
            ++applyFailures;
            if (!firstFailure)
            {
                firstFailure = std::move(error);
            }
            continue;
        }
        log::error("{} failed: {}; undoing", operation, error.toString());
        const auto notUndone = undo(beforeModules, desiredAll, touched);
        const JournalEntry entry{std::string(operation), snapshotId, desiredAll, 0};
        if (auto closed = closeUndone(entry, beforeModules, label, notUndone, false); !closed)
        {
            log::error("cannot close the journal after the undo ({}); it is undone again on the next start",
                       closed.error().toString());
        }
        if (!notUndone.empty())
        {
            return std::unexpected(std::move(error).withContext(std::format(
                "undo also failed for {}; restore the original to put it back", moduleNames(notUndone))));
        }
        return std::unexpected(std::move(error));
    }

    if (firstFailure && applyFailures == steps.size())
    {
        // Nothing came back: nothing to record.
        if (auto transaction = m_state.transaction())
        {
            const bool closed = m_state.closeJournal() && m_state.deleteSnapshot(snapshotId);
            if (!closed || !transaction->commit())
            {
                log::error("cannot close the journal; the operation will be finished on the next start");
            }
        }
        return std::unexpected(std::move(*firstFailure));
    }

    // 5. lastApplied per field, history, journal closed — in one database transaction
    auto historyId = recordOperation(label, steps, update, snapshotId, automatic, report);
    if (!historyId)
    {
        return propagate(historyId);
    }
    report.historyId = *historyId;
    collectGarbage(report);
    backUpOriginal(); // what we hold changed
    return report;
}

Result<std::int64_t> RiceEngine::recordOperation(const std::string& label, std::span<const Step> steps,
                                                 AppliedUpdate update, std::int64_t snapshotId, bool automatic,
                                                 OperationReport& report)
{
    const auto failed = [&](const std::string& id) {
        return std::ranges::any_of(report.failed, [&](const FailedModule& entry) { return entry.module == id; });
    };
    std::vector<std::pair<std::string, Json>> appliedUpdates;
    for (const auto& step : steps)
    {
        const std::string id(step.module->id());
        if (failed(id))
        {
            continue; // still ours: the next restore tries again
        }
        auto existing = m_state.applied(id);
        if (!existing)
        {
            return propagate(existing);
        }
        // What Windows actually holds now (it may normalize what we wrote), in the shape of every other
        // capture (markers such as "keep" resolved), so later three-way comparisons compare like with like.
        auto after = captureInternalized(*step.module);
        const auto shown = [&](const std::string& field) {
            return after && after->state.is_object() && after->state.contains(field);
        };
        for (const auto& [field, value] : step.desired.items())
        {
            if (update == AppliedUpdate::Merge)
            {
                (*existing)[field] = shown(field) ? after->state[field] : value;
            }
            else if (after && !shown(field))
            {
                // Windows does not show it now (a monitor that is not connected): it did not come back.
                // It stays ours with its original value, which comes back when it is there again.
                (*existing)[field] = value;
                report.skipped.push_back({id, field, "not there now; it comes back when it is"});
            }
            else
            {
                existing->erase(field);
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
    std::int64_t historyId = 0;
    if (!automatic)
    {
        auto added = m_state.addHistory(snapshotId, label, kHistoryLimit);
        if (!added)
        {
            return propagate(added);
        }
        historyId = *added;
    }
    if (auto closed = m_state.closeJournal(); !closed)
    {
        return propagate(closed);
    }
    if (automatic)
    {
        // Nothing will go back to this snapshot: drop it with the journal.
        if (auto dropped = m_state.deleteSnapshot(snapshotId); !dropped)
        {
            return propagate(dropped);
        }
    }
    if (update == AppliedUpdate::Remove && report.failed.empty())
    {
        // The original came back: whatever an interrupted operation left behind with it.
        if (auto cleared = m_state.setMeta(kNeedsRestore, std::nullopt); !cleared)
        {
            return propagate(cleared);
        }
    }
    if (auto committed = transaction->commit(); !committed)
    {
        return propagate(committed);
    }
    return historyId;
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
