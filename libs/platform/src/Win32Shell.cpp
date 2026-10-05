#include "rmw/base/Utf.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shellapi.h>

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
    if (SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) == FALSE)
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
    if (SystemParametersInfoW(kSpiSetCursorBaseSize, 0, size, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) == FALSE)
    {
        return std::unexpected(lastError("set cursor size"));
    }
    return {};
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
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (data.hWnd == nullptr)
    {
        return fail(ErrorCode::NotFound, "the taskbar (Shell_TrayWnd) is not running");
    }
    data.lParam = enabled ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &data);
    return {};
}

} // namespace rmw::platform
