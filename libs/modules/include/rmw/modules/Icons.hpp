#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/ports/Icons.hpp"
#include "rmw/ports/Registry.hpp"

// The icon pack (M14, ADR-0021), per user. Fields:
//   "this-pc", "recycle-bin", "recycle-bin-full", "network", "user-files", "control-panel"
//       the desktop icons (each CLSID's DefaultIcon key under HKCU\...\Explorer\CLSID):
//       {"type": "expand"|"string", "data": "C:\x.ico,0"} or null (absent)
//   "recycle-bin-now"   the Recycle Bin's icon now (the key's default value), same shape
//   "folder:<name>"     a user folder's icon (desktop.ini): "C:\x.ico,0", {"data": "C:\x.ico,0"} or null
//   "apps"              the user's shortcuts (Start, desktop, taskbar). From a rice:
//                       {"byProgram": {"firefox.exe": "C:\x.ico,0"}}; captured:
//                       {"byShortcut": {"<.lnk path>": {"program": "firefox.exe", "icon": ""}}}
//                       (icon "": the program's own)
//   "type:<ext>"        a file type's icon: {"progId": "txtfile", "icon": {"type", "data"} or null},
//                       HKCU\Software\Classes\<ProgID>\DefaultIcon. From a rice: {"icon": ...}; the
//                       ProgID is found when it is applied (a restore uses the one captured)
// Every .ico is checked by validateIconFile() before it reaches the store. A captured location of a
// user's .ico keeps its path and gains "copy" (the same icon in the store): a restore puts the path
// back while the file is there (a shortcut keeps pointing at its app's own .ico, not at our store),
// and the copy when it is gone.
namespace rmw::modules
{

class IconsLogic final : public engine::IModuleLogic
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "icons"; }
    [[nodiscard]] Result<engine::Json> desiredFromRice(const domain::Rice& rice,
                                                       engine::IAssetSink& assets) const override;
    // Copies .ico files outside %SystemRoot% into the store, as "copy" next to the location (a
    // restore still works after the user deletes their old icon pack).
    [[nodiscard]] Result<void> internalize(engine::Json& state, engine::IAssetSink& assets) const override;
    // "apps" by program and "apps" by shortcut are the same when every shortcut of those programs
    // has that icon. "copy" never counts.
    [[nodiscard]] bool fieldEquivalent(std::string_view field, const engine::Json& a,
                                       const engine::Json& b) const override;
};

class IconsSystem final : public engine::IModuleSystem
{
public:
    IconsSystem(ports::IRegistry& registry, ports::IIcons& icons) noexcept
        : m_registry(registry)
        , m_icons(icons)
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return "icons"; }
    [[nodiscard]] Result<engine::CaptureResult> capture() override;
    // The fixed fields, and the file types asked for ("type:txt").
    [[nodiscard]] Result<engine::CaptureResult> captureFields(std::span<const std::string> fields) override;
    [[nodiscard]] Result<void> apply(const engine::Json& desired) override;

private:
    ports::IRegistry& m_registry;
    ports::IIcons& m_icons;
};

} // namespace rmw::modules
