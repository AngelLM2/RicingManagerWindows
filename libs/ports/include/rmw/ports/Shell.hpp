#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <string_view>

namespace rmw::ports
{

// Notifications and live settings that go through the shell. Every call that may reach
// Explorer is bounded (SendMessageTimeout with SMTO_ABORTIFHUNG).
class IShell
{
public:
    virtual ~IShell() = default;

    // WM_SETTINGCHANGE to every top-level window: "ImmersiveColorSet", "TraySettings", ...
    [[nodiscard]] virtual Result<void> broadcastSettingChange(std::string_view area) = 0;

    // Reloads the cursor scheme from HKCU\Control Panel\Cursors (SPI_SETCURSORS).
    [[nodiscard]] virtual Result<void> reloadCursors() = 0;
    // Cursor size in pixels (32..256, step 16), applied live.
    [[nodiscard]] virtual Result<void> setCursorSize(std::uint32_t pixels) = 0;

    // True for files Windows ships (under the Windows folder, %VARS% expanded): a snapshot
    // keeps pointing at them instead of copying them.
    [[nodiscard]] virtual bool isWindowsFile(std::string_view file) = 0;
    // Whether a file is there (%VARS% expanded): a captured file may have been deleted since.
    [[nodiscard]] virtual bool fileExists(std::string_view file) = 0;

    // Native taskbar auto-hide (SHAppBarMessage ABM_GETSTATE / ABM_SETSTATE).
    [[nodiscard]] virtual Result<bool> taskbarAutoHide() = 0;
    [[nodiscard]] virtual Result<void> setTaskbarAutoHide(bool enabled) = 0;
};

// The look of Windows as a .theme expresses it (ADR-0005).
struct ThemeLook
{
    bool appsLight = true;
    bool systemLight = true;
    std::uint32_t colorizationArgb = 0xC40078D4; // 0xAARRGGBB

    friend bool operator==(const ThemeLook&, const ThemeLook&) = default;
};

class IThemeApi
{
public:
    virtual ~IThemeApi() = default;

    // Writes the managed theme (based on the active one, so cursors, sounds and wallpaper stay)
    // with this look and applies it, ignoring wallpaper, cursors, icons, sounds and screensaver.
    // Returns once Windows reports the look, or Timeout.
    [[nodiscard]] virtual Result<void> applyLook(const ThemeLook& look) = 0;
    // Applies a theme file of the user's again (a restore: the theme they had selected), its look only
    // where Windows allows. Neither call changes anything but the look: what a theme apply changes in the
    // pointers, the wallpaper, the sounds, the screen saver, the desktop icons or Settings' recent colors
    // is put back.
    [[nodiscard]] virtual Result<void> applyThemeFile(const std::string& file) = 0;
    // Names a theme file of the user's as the current one (Settings shows it selected) without applying
    // it: after the look it carries was set another way.
    [[nodiscard]] virtual Result<void> nameCurrentTheme(const std::string& file) = 0;
    // Removes our managed theme files when neither is the current theme (after a restore named the
    // user's again): Settings no longer lists "Ricing Manager".
    virtual void forgetManagedThemes() = 0;
};

} // namespace rmw::ports
