#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

#include <functional>

// Light/dark mode, accent color, transparency and accent on taskbar (ADR-0005). Fields:
//   "appsLight", "systemLight"  bool   — via the managed .theme (IThemeManager2)
//   "accent"                    "#RRGGBB" — via the managed .theme (ColorizationColor)
//   "transparency"              bool   — Personalize\EnableTransparency
//   "accentOnTaskbar"           bool   — Personalize\ColorPrevalence
namespace rmw::modules
{

class AppearanceLogic final : public engine::IModuleLogic
{
public:
    // `now` gives the local time of day, for mode = "schedule" (the scheduler keeps it in sync
    // later; applying picks the period we are in).
    explicit AppearanceLogic(std::function<domain::TimeOfDay()> now = {});

    [[nodiscard]] std::string_view id() const noexcept override { return "appearance"; }
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    [[nodiscard]] bool fieldEquivalent(std::string_view field, const engine::Json& a,
                                       const engine::Json& b) const override;

private:
    std::function<domain::TimeOfDay()> m_now;
};

class AppearanceSystem final : public engine::IModuleSystem
{
public:
    AppearanceSystem(ports::IRegistry& registry, ports::IShell& shell, ports::IThemeApi& theme) noexcept
        : m_registry(registry)
        , m_shell(shell)
        , m_theme(theme)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "appearance"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IShell& m_shell;
    ports::IThemeApi& m_theme;
};

// True when `now` falls in the light period of `schedule` (handles periods across midnight).
[[nodiscard]] bool isLightPeriod(const domain::AppearanceSchedule& schedule, domain::TimeOfDay now) noexcept;

} // namespace rmw::modules
