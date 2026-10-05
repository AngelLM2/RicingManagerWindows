#pragma once

#include "rmw/ports/Display.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

// Win32 implementations of rmw::ports.
namespace rmw::platform
{

// HKCU, optionally redirected under HKCU\<root> so system tests never touch real settings.
class Win32Registry final : public ports::IRegistry
{
public:
    // `root` empty = the real HKCU; otherwise a subkey such as
    // R"(Software\RicingManagerWindows\Tests\<random>)".
    explicit Win32Registry(std::wstring root = {});

    [[nodiscard]] Result<std::optional<ports::RegistryValue>> read(std::string_view key,
                                                                   std::string_view name) override;
    [[nodiscard]] Result<void> write(std::string_view key, std::string_view name,
                                     const ports::RegistryValue& value) override;
    [[nodiscard]] Result<void> remove(std::string_view key, std::string_view name) override;

    // Deletes the redirected root and everything below it (no-op for the real HKCU).
    [[nodiscard]] Result<void> deleteRoot();

private:
    [[nodiscard]] Result<std::wstring> fullKey(std::string_view key) const;

    std::wstring m_root;
};

class Win32Shell final : public ports::IShell
{
public:
    explicit Win32Shell(std::chrono::milliseconds broadcastTimeout = std::chrono::seconds(5)) noexcept
        : m_timeout(broadcastTimeout)
    {
    }

    [[nodiscard]] Result<void> broadcastSettingChange(std::string_view area) override;
    [[nodiscard]] Result<void> reloadCursors() override;
    [[nodiscard]] Result<void> setCursorSize(std::uint32_t pixels) override;
    [[nodiscard]] bool isWindowsFile(std::string_view file) override;
    [[nodiscard]] Result<bool> taskbarAutoHide() override;
    [[nodiscard]] Result<void> setTaskbarAutoHide(bool enabled) override;

private:
    std::chrono::milliseconds m_timeout;
};

// The managed-theme sequence of ADR-0005 on IThemeManager2 (undocumented COM), with the legacy
// IThemeManager as fallback. Each call runs on its own STA thread.
class Win32ThemeApi final : public ports::IThemeApi
{
public:
    explicit Win32ThemeApi(std::chrono::milliseconds wait = std::chrono::seconds(10)) noexcept
        : m_wait(wait)
    {
    }

    [[nodiscard]] Result<void> applyLook(const ports::ThemeLook& look) override;

private:
    std::chrono::milliseconds m_wait;
};

// QueryDisplayConfig + EDID from the registry (read-only, never reaches Explorer).
class Win32DisplayTopology final : public ports::IDisplayTopology
{
public:
    [[nodiscard]] Result<std::vector<ports::MonitorInfo>> monitors() override;
};

// IDesktopWallpaper; every call runs on its own STA thread.
class Win32WallpaperApi final : public ports::IWallpaperApi
{
public:
    [[nodiscard]] Result<std::vector<ports::WallpaperMonitor>> monitors() override;
    [[nodiscard]] Result<std::string> wallpaper(std::string_view devicePath) override;
    [[nodiscard]] Result<void> setWallpaper(std::string_view devicePath, std::string_view file) override;
    [[nodiscard]] Result<ports::WallpaperPosition> position() override;
    [[nodiscard]] Result<void> setPosition(ports::WallpaperPosition position) override;
    [[nodiscard]] Result<std::uint32_t> backgroundColor() override;
    [[nodiscard]] Result<void> setBackgroundColor(std::uint32_t rgb) override;
    [[nodiscard]] Result<ports::SlideshowState> slideshow() override;
    [[nodiscard]] Result<void> setSlideshow(const ports::SlideshowState& state) override;
    [[nodiscard]] Result<std::string> transcodedWallpaper() override;
    [[nodiscard]] Result<std::string> sniffImageType(std::string_view file) override;
    [[nodiscard]] bool isWindowsFile(std::string_view file) override;
};

// The identity key of ADR-0004 from EDID fields (pure, testable). `serialIsUnique` says whether
// no other connected monitor has the same manufacturer/product/serial; `targetId` is the
// display target (the "UID<n>" of the monitor's device instance), used when the serial cannot
// tell monitors apart.
struct EdidIdentity
{
    std::string manufacturer; // 3-letter PNP id; empty when there is no EDID
    std::uint16_t product = 0;
    std::uint32_t serialNumber = 0;
    std::string serialText;
    std::string name;
};
[[nodiscard]] std::string monitorIdentityKey(const EdidIdentity& edid, std::string_view devicePath,
                                             std::uint32_t targetId, bool serialIsUnique);
// Manufacturer and product from QueryDisplayConfig's EDID ids (the manufacturer arrives
// byte-swapped there); serial fields stay empty.
[[nodiscard]] EdidIdentity edidFromIds(std::uint16_t manufactureId, std::uint16_t productCode);
[[nodiscard]] EdidIdentity parseEdid(std::span<const std::byte> edid);

// Builds the managed .theme text from a base theme (pure, testable): sets [Theme] DisplayName and
// [VisualStyles] SystemMode/AppMode/ColorizationColor, adding them when missing, keeping every
// other line as it was.
[[nodiscard]] std::wstring buildManagedTheme(std::wstring_view baseTheme, const ports::ThemeLook& look);

inline constexpr std::wstring_view kManagedThemeName = L"Ricing Manager";

} // namespace rmw::platform
