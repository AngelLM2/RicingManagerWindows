#include "Desktop.hpp"

#include <windows.h>

#include <format>

namespace rmw::wallpaper
{
namespace
{

constexpr UINT kSpawnWorker = 0x052C; // Progman: create the WorkerW behind the icons
constexpr UINT kSpawnTimeoutMs = 1000;

HWND iconHost()
{
    HWND host = nullptr;
    EnumWindows(
        [](HWND top, LPARAM param) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
            {
                *reinterpret_cast<HWND*>(param) = top; // NOLINT(performance-no-int-to-ptr)
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&host));
    return host;
}

} // namespace

// The window keeps its visibility: a new one is shown with its first frame.
Result<Attachment> attachBehindIcons(void* handle, int left, int top, int width, int height)
{
    auto* const window = static_cast<HWND>(handle);
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman == nullptr)
    {
        return fail(ErrorCode::NotFound, "the desktop (Progman) is not there: Explorer is not running");
    }
    const bool raised = (GetWindowLongPtrW(progman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(progman, kSpawnWorker, 0xD, 0x1, SMTO_NORMAL | SMTO_ABORTIFHUNG, kSpawnTimeoutMs,
                        &ignored);

    SetWindowLongPtrW(window, GWL_STYLE, (GetWindowLongPtrW(window, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    if (raised)
    {
        HWND icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (icons == nullptr)
        {
            return fail(ErrorCode::NotFound, "the desktop icons (SHELLDLL_DefView) are not there");
        }
        // A child of a redirection-less window draws only as a layered window.
        SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
        SetParent(window, progman);
        POINT origin{left, top};
        MapWindowPoints(HWND_DESKTOP, progman, &origin, 1);
        SetWindowPos(window, icons, origin.x, origin.y, width, height, SWP_NOACTIVATE);
        // Explorer's own wallpaper worker goes to the back, behind ours.
        if (HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr))
        {
            SetWindowPos(worker, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (GetParent(window) != progman)
        {
            return fail(ErrorCode::System, "could not go behind the desktop icons");
        }
        return Attachment{progman, true};
    }

    HWND host = iconHost();
    HWND worker = host != nullptr ? FindWindowExW(nullptr, host, L"WorkerW", nullptr) : nullptr;
    if (worker == nullptr)
    {
        return fail(ErrorCode::NotFound, "the desktop's wallpaper window (WorkerW) is not there");
    }
    SetParent(window, worker);
    POINT origin{left, top};
    MapWindowPoints(HWND_DESKTOP, worker, &origin, 1);
    SetWindowPos(window, HWND_TOP, origin.x, origin.y, width, height, SWP_NOACTIVATE);
    if (GetParent(window) != worker)
    {
        return fail(ErrorCode::System, "could not go behind the desktop icons");
    }
    return Attachment{worker, false};
}

bool stillAttached(void* handle, const Attachment& attachment)
{
    auto* const window = static_cast<HWND>(handle);
    auto* const parent = static_cast<HWND>(attachment.parent);
    if (IsWindow(window) == FALSE || IsWindow(parent) == FALSE || GetParent(window) != parent)
    {
        return false;
    }
    if (attachment.raised)
    {
        // Below the icons, with only our own windows (other monitors) between: anything else there
        // would hide them or us.
        HWND icons = FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr);
        for (HWND above = GetWindow(window, GW_HWNDPREV); above != nullptr;
             above = GetWindow(above, GW_HWNDPREV))
        {
            if (above == icons)
            {
                return true;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(above, &pid);
            if (pid != GetCurrentProcessId())
            {
                return false;
            }
        }
        return false;
    }
    return true;
}

} // namespace rmw::wallpaper
