#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

// The sound scheme (M14, ADR-0021): the file of each sound event, at
// HKCU\AppEvents\Schemes\Apps\<app>\<event>\.Current (the key's default value), and the scheme's name
// (HKCU\AppEvents\Schemes' default value: ".Default", ".None", ".Current" when changed). Fields:
//   "beep", "asterisk", ... "navigating"   {"type": "expand"|"string", "data": path} or null (absent);
//                                          "data" empty: no sound for that event
//   "scheme"                               the scheme's name, same shape or null
//   "others"                               every other event Windows knows ("Apps\<app>\<event>" -> the
//                                          same shape), or "silent" from a rice's "no sounds" (all of them)
// Every file is checked by validateWaveFile() before it reaches the store: Windows plays them in
// many processes.
namespace rmw::modules
{

class SoundsLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "sounds"; }
    [[nodiscard]] std::span<const std::string_view> dependsOn() const noexcept override;
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    // Copies sound files outside %SystemRoot% into the store, so a restore still works after the
    // user deletes them.
    [[nodiscard]] Result<void> internalize(engine::Json& state, engine::IAssetSink& assets) const override;
    // "copy" never counts.
    [[nodiscard]] bool fieldEquivalent(std::string_view field, const engine::Json& a,
                                       const engine::Json& b) const override;
};

class SoundsSystem final : public engine::IModuleSystem
{
public:
    SoundsSystem(ports::IRegistry& registry, ports::IShell& shell) noexcept
        : m_registry(registry)
        , m_shell(shell)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "sounds"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IShell& m_shell;
};

} // namespace rmw::modules
