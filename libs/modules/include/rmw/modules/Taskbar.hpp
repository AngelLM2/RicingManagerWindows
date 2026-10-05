#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

// The NATIVE taskbar, without injection (ADR-0003, docs/09 §5): alignment, Task View button,
// Search mode and auto-hide. Fields:
//   "alignment"  TaskbarAl DWORD (0 left, 1 center) or null — applies after Explorer restarts
//   "taskView"   ShowTaskViewButton DWORD (0/1) or null      — live
//   "search"     SearchboxTaskbarMode DWORD (0 hidden, 1 icon, 2 box, 3 button) or null — live
//   "autohide"   bool                                         — live
namespace rmw::modules
{

class TaskbarLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "taskbar"; }
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
};

class TaskbarSystem final : public engine::IModuleSystem
{
public:
    TaskbarSystem(ports::IRegistry& registry, ports::IShell& shell) noexcept
        : m_registry(registry)
        , m_shell(shell)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "taskbar"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IShell& m_shell;
};

} // namespace rmw::modules
