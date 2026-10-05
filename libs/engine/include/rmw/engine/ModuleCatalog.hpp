#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/engine/Module.hpp"

#include <string_view>
#include <vector>

namespace rmw::engine
{

// The registered module logic, in dependency order (docs/03 §4: appearance before wallpaper and
// cursors, because applying a .theme can touch them).
class ModuleCatalog
{
public:
    // Ids are lower-case [a-z][a-z0-9-]*, unique. The module must outlive the catalog.
    [[nodiscard]] Result<void> add(const IModuleLogic& module);

    // Orders the modules so each comes after what it depends on (registration order otherwise).
    // Unknown dependencies and cycles are errors. Call once, after every add().
    [[nodiscard]] Result<void> finalize();

    [[nodiscard]] const std::vector<const IModuleLogic*>& ordered() const noexcept { return m_ordered; }
    [[nodiscard]] const IModuleLogic* find(std::string_view id) const noexcept;

private:
    std::vector<const IModuleLogic*> m_added;
    std::vector<const IModuleLogic*> m_ordered;
};

// Runs the system halves in this process (tests, and modules that never block on Explorer).
class LocalModuleRunner final : public IModuleRunner
{
public:
    void add(IModuleSystem& system) { m_systems.push_back(&system); }

    [[nodiscard]] Result<CaptureResult> capture(std::string_view moduleId) override;
    [[nodiscard]] Result<void> apply(std::string_view moduleId, const Json& desired) override;

private:
    [[nodiscard]] Result<IModuleSystem*> find(std::string_view moduleId) const;

    std::vector<IModuleSystem*> m_systems;
};

} // namespace rmw::engine
