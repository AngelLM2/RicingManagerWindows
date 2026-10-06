#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/NativeTaskbar.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shellapi.h>

#include <array>
#include <format>

namespace rmw::platform
{
namespace
{

// Not in the SDK headers: sets the cursor base size live (docs/09 §6; the value Settings uses).
constexpr UINT kSpiSetCursorBaseSize = 0x2029;

} // namespace

Result<void> Win32Shell::broadcastSettingChange(std::string_view area)
{
    auto wideArea = utf8To16(area);
    if (!wideArea)
    {
        return std::unexpected(std::move(wideArea.error()));
    }
    DWORD_PTR ignored = 0;
    // ABORTIFHUNG: a hung window must not hang us (docs/03 §5).
    const LRESULT sent =
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            reinterpret_cast<LPARAM>(wideArea->empty() ? nullptr : wideArea->c_str()),
                            SMTO_ABORTIFHUNG, static_cast<UINT>(m_timeout.count()), &ignored);
    if (sent == 0)
    {
        // A timeout only means some window was slow; the rest got the message.
        const DWORD error = GetLastError();
        if (error != ERROR_TIMEOUT && error != ERROR_SUCCESS)
        {
            return std::unexpected(win32Error(error, std::format("broadcast WM_SETTINGCHANGE '{}'", area)));
        }
    }
    return {};
}

Result<void> Win32Shell::reloadCursors()
{
    // The values are already in the registry. With SPIF_UPDATEINIFILE | SPIF_SENDCHANGE this call
    // returns FALSE with no error even though it worked (measured in Windows Sandbox, 25H2), so
    // it is made without flags.
    if (SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0) == FALSE)
    {
        return std::unexpected(lastError("SPI_SETCURSORS"));
    }
    return {};
}

Result<void> Win32Shell::setCursorSize(std::uint32_t pixels)
{
    if (pixels < 32 || pixels > 256 || pixels % 16 != 0)
    {
        return fail(ErrorCode::InvalidArgument,
                    std::format("cursor size {} is not 32..256 in steps of 16", pixels));
    }
    // The size goes in pvParam itself, not behind a pointer.
    auto* const size =
        reinterpret_cast<PVOID>(static_cast<UINT_PTR>(pixels)); // NOLINT(performance-no-int-to-ptr)
    if (SystemParametersInfoW(kSpiSetCursorBaseSize, 0, size, 0) ==
        FALSE) // the module writes CursorBaseSize itself
    {
        return std::unexpected(lastError("set cursor size"));
    }
    return {};
}

bool Win32Shell::fileExists(std::string_view file)
{
    auto wide = utf8To16(file);
    if (!wide || wide->empty())
    {
        return false;
    }
    std::array<wchar_t, 32'768> expanded{};
    const DWORD length = ExpandEnvironmentStringsW(wide->c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    return length > 0 && length <= expanded.size() && GetFileAttributesW(expanded.data()) != INVALID_FILE_ATTRIBUTES;
}

bool Win32Shell::isWindowsFile(std::string_view file)
{
    auto wide = utf8To16(file);
    if (!wide)
    {
        return false;
    }
    std::array<wchar_t, 32'768> expanded{};
    const DWORD length =
        ExpandEnvironmentStringsW(wide->c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT windowsLength = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    return length > 0 && length <= expanded.size() && windowsLength > 0 &&
           isWithin(std::wstring(expanded.data()), std::wstring(windows.data(), windowsLength));
}

Result<bool> Win32Shell::taskbarAutoHide()
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    const auto state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
    return (state & ABS_AUTOHIDE) != 0;
}

Result<void> Win32Shell::setTaskbarAutoHide(bool enabled)
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(nativeTrayWindow());
    if (data.hWnd == nullptr)
    {
        return fail(ErrorCode::NotFound, "the taskbar (Shell_TrayWnd) is not running");
    }
    data.lParam = enabled ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &data);
    return {};
}

} // namespace rmw::platform
