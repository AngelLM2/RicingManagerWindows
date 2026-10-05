#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

#include <span>
#include <string>

// Tweak plugins (P1, docs/12 §3): allow-listed HKEY_CURRENT_USER values, applied and undone like any
// module. A field per value, named "<key>|<value name>":
//   {"type": "dword", "data": 1} | {"type": "string", "data": "text"} | null (the value is absent)
// The fields are not fixed, so this module captures only the fields it is asked for. Every key,
// name and string is checked again here (rmw::domain Tweaks.hpp): a field never reaches the
// registry because a manifest once passed.
namespace rmw::modules
{

class TweaksLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "tweaks"; }
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
};

class TweaksSystem final : public engine::IModuleSystem
{
public:
    TweaksSystem(ports::IRegistry& registry, ports::IShell& shell) noexcept
        : m_registry(registry)
        , m_shell(shell)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "tweaks"; }
    // Nothing without a list of fields.
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<engine::CaptureResult> captureFields(std::span<const std::string> fields) override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IShell& m_shell;
};

// "<key>|<value name>" for a tweak, and back (checked).
[[nodiscard]] std::string tweakField(std::string_view key, std::string_view value);
struct TweakTarget
{
    std::string key;
    std::string value;
};
[[nodiscard]] Result<TweakTarget> parseTweakField(std::string_view field);

} // namespace rmw::modules
