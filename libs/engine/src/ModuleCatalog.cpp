#include "rmw/engine/ModuleCatalog.hpp"

#include "rmw/domain/Identifiers.hpp"

#include <algorithm>
#include <format>

namespace rmw::engine
{

Result<void> ModuleCatalog::add(const IModuleLogic& module)
{
    const std::string_view id = module.id();
    if (!domain::isValidBuiltinName(id) || id.find('.') != std::string_view::npos)
    {
        return fail(ErrorCode::InvalidArgument, std::format("invalid module id '{}'", id));
    }
    if (find(id) != nullptr || std::ranges::any_of(m_added, [&](auto* m) { return m->id() == id; }))
    {
        return fail(ErrorCode::AlreadyExists, std::format("module '{}' registered twice", id));
    }
    m_added.push_back(&module);
    return {};
}

Result<void> ModuleCatalog::finalize()
{
    const auto byId = [&](std::string_view id) -> const IModuleLogic* {
        const auto it = std::ranges::find_if(m_added, [&](auto* m) { return m->id() == id; });
        return it == m_added.end() ? nullptr : *it;
    };
    for (const auto* module : m_added)
    {
        for (const auto dependency : module->dependsOn())
        {
            if (byId(dependency) == nullptr)
            {
                return fail(
                    ErrorCode::InvalidArgument,
                    std::format("module '{}' depends on unknown module '{}'", module->id(), dependency));
            }
        }
    }

    // Kahn's algorithm, picking ready modules in registration order for a stable result.
    std::vector<const IModuleLogic*> ordered;
    std::vector<const IModuleLogic*> pending = m_added;
    const auto placed = [&](std::string_view id) {
        return std::ranges::any_of(ordered, [&](auto* m) { return m->id() == id; });
    };
    while (!pending.empty())
    {
        const auto ready = std::ranges::find_if(pending, [&](auto* module) {
            return std::ranges::all_of(module->dependsOn(), [&](std::string_view d) { return placed(d); });
        });
        if (ready == pending.end())
        {
            return fail(ErrorCode::InvalidArgument,
                        std::format("module dependency cycle involving '{}'", pending.front()->id()));
        }
        ordered.push_back(*ready);
        pending.erase(ready);
    }
    m_ordered = std::move(ordered);
    return {};
}

const IModuleLogic* ModuleCatalog::find(std::string_view id) const noexcept
{
    const auto it = std::ranges::find_if(m_ordered, [&](auto* m) { return m->id() == id; });
    return it == m_ordered.end() ? nullptr : *it;
}

Result<IModuleSystem*> LocalModuleRunner::find(std::string_view moduleId) const
{
    const auto it = std::ranges::find_if(m_systems, [&](auto* s) { return s->id() == moduleId; });
    if (it == m_systems.end())
    {
        return fail(ErrorCode::NotFound, std::format("no system half for module '{}'", moduleId));
    }
    return *it;
}

Result<CaptureResult> LocalModuleRunner::capture(std::string_view moduleId)
{
    auto system = find(moduleId);
    if (!system)
    {
        return std::unexpected(std::move(system.error()));
    }
    return (*system)->capture();
}

Result<void> LocalModuleRunner::apply(std::string_view moduleId, const Json& desired)
{
    auto system = find(moduleId);
    if (!system)
    {
        return std::unexpected(std::move(system.error()));
    }
    return (*system)->apply(desired);
}

} // namespace rmw::engine
