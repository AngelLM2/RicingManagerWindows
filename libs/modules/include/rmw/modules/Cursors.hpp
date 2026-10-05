#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

// The cursor scheme (docs/09 §6): REG_EXPAND_SZ values under HKCU\Control Panel\Cursors plus
// SPI_SETCURSORS. Fields:
//   "Arrow", "Help", ... "Person"   {"type": "expand"|"string", "data": path} or null (absent)
//   "scheme"                         the key's default value (scheme name), same shape or null
//   "schemeSource"                   "Scheme Source" DWORD or null
//   "size"                           CursorBaseSize DWORD (32..256) or null
// Every file is checked by validateCursorFile() before it reaches the store: once applied, all
// processes on the desktop parse it (docs/06 §4).
namespace rmw::modules
{

class CursorsLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "cursors"; }
    [[nodiscard]] std::span<const std::string_view> dependsOn() const noexcept override;
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    // Copies cursor files outside %SystemRoot% into the store, so a restore still works after the
    // user deletes the pack they had installed.
    [[nodiscard]] Result<void> internalize(engine::Json& state, engine::IAssetSink& assets) const override;
};

class CursorsSystem final : public engine::IModuleSystem
{
public:
    CursorsSystem(ports::IRegistry& registry, ports::IShell& shell) noexcept
        : m_registry(registry)
        , m_shell(shell)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "cursors"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IShell& m_shell;
};

} // namespace rmw::modules
