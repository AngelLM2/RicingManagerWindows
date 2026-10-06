#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/LockScreen.hpp"
#include "rmw/ports/Registry.hpp"

// The lock screen (M14, ADR-0021), per user. Fields:
//   "picture"       {"spotlight": true|false|null, "file": "C:\x.jpg"} — Windows Spotlight
//                   (HKCU\...\ContentDeliveryManager RotatingLockScreenEnabled) and the picture
//                   (LockScreen API). One field: a restore brings both back together. From a rice:
//                   {"spotlight": false, "file": <store>} or {"spotlight": true}. A captured user
//                   picture gains "copy" (the same file in the store), used when the file is gone.
//   "tips"          RotatingLockScreenOverlayEnabled: true|false|null (absent)
//   "tips-content"  SubscribedContent-338387Enabled: true|false|null — Settings' "fun facts, tips,
//                   tricks and more on your lock screen" writes both
// Pictures are .jpg or .png, their declared size checked before they reach the store.
namespace rmw::modules
{

class LockScreenLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "lockscreen"; }
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    // A picture outside %SystemRoot% gets a copy in the store, next to its path.
    [[nodiscard]] Result<void> internalize(engine::Json& state, engine::IAssetSink& assets) const override;
    // "picture": the same Spotlight choice, and the same file when the rice names one ("copy" never
    // counts; with Spotlight on, its pictures change by themselves).
    [[nodiscard]] bool fieldEquivalent(std::string_view field, const engine::Json& a,
                                       const engine::Json& b) const override;
};

class LockScreenSystem final : public engine::IModuleSystem
{
public:
    LockScreenSystem(ports::IRegistry& registry, ports::ILockScreen& lockScreen) noexcept
        : m_registry(registry)
        , m_lockScreen(lockScreen)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "lockscreen"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::ILockScreen& m_lockScreen;
};

} // namespace rmw::modules
