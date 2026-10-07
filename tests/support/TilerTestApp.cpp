// A plain app window for the M13 Sandbox tests (tests/system/sandbox/tiler-tests.ps1):
//   rmw_tiler_test_app <title> [--fixed] [--dialog] [--min <w>x<h>] [--fullscreen-after <ms>]
//                      [--tray-after <ms>] [--maximized]
//   rmw_tiler_test_app --medium <command line>
// A resizable window (or a fixed-size one) titled <title>, painted in a color of its own; with
// --dialog it also opens a window it owns (a dialog that must hide and show with it). With --medium it
// starts <command line> at medium integrity (as an app is on a PC with the UAC on) and leaves: the
// Sandbox runs everything as administrator. v1.8: --min refuses to be made smaller than w x h pixels
// (Discord, Spotify); --fullscreen-after goes full screen without a frame later (a browser's F11, a
// game); --tray-after hides the window later, as an app closed to the tray does.

#include <windows.h>

#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

namespace
{

constexpr wchar_t kClass[] = L"RmwTilerTestWindow";
constexpr UINT_PTR kFullscreenTimer = 1;
constexpr UINT_PTR kTrayTimer = 2;

POINT g_minimum{0, 0}; // --min, for the main window

void goFullscreen(HWND window)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(style & ~(WS_CAPTION | WS_THICKFRAME)));
    const RECT& m = info.rcMonitor;
    SetWindowPos(window, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_GETMINMAXINFO:
        if (GetWindow(window, GW_OWNER) == nullptr && (g_minimum.x > 0 || g_minimum.y > 0))
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam); // NOLINT(performance-no-int-to-ptr)
            info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, g_minimum.x);
            info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, g_minimum.y);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_TIMER:
        KillTimer(window, wParam);
        if (wParam == kFullscreenTimer)
        {
            goFullscreen(window);
        }
        else if (wParam == kTrayTimer)
        {
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const auto seed = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(window) >> 4U);
        HBRUSH brush = CreateSolidBrush(RGB(60 + (seed % 150), 60 + ((seed / 7) % 150), 120 + ((seed / 13) % 120)));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (GetWindow(window, GW_OWNER) == nullptr)
        {
            PostQuitMessage(0);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

// A copy of this process's token lowered to medium integrity: no privilege is needed to start a
// process with a token that has less than one's own.
int startMedium(const wchar_t* commandLine)
{
    HANDLE own = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own) == FALSE)
    {
        return 1;
    }
    HANDLE medium = nullptr;
    const BOOL duplicated = DuplicateTokenEx(own, 0, nullptr, SecurityImpersonation, TokenPrimary, &medium);
    CloseHandle(own);
    PSID sid = nullptr;
    if (duplicated == FALSE || ConvertStringSidToSidW(L"S-1-16-8192", &sid) == FALSE) // medium
    {
        return 1;
    }
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = sid;
    const BOOL lowered =
        SetTokenInformation(medium, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid));
    LocalFree(sid);
    std::wstring line = commandLine;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = lowered != FALSE && CreateProcessAsUserW(medium, nullptr, line.data(), nullptr, nullptr, FALSE,
                                                                  0, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(medium);
    if (!started)
    {
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR commandLine, int /*show*/)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (const std::wstring_view line = commandLine; line.starts_with(L"--medium "))
    {
        return startMedium(commandLine + 9);
    }
    int count = 0;
    wchar_t** args = CommandLineToArgvW(commandLine, &count);
    std::wstring title = L"Tiler test";
    bool fixed = false;
    bool dialog = false;
    UINT fullscreenAfter = 0;
    UINT trayAfter = 0;
    bool maximized = false;
    for (int i = 0; args != nullptr && i < count; ++i)
    {
        const bool more = i + 1 < count;
        if (std::wcscmp(args[i], L"--fixed") == 0)
        {
            fixed = true;
        }
        else if (std::wcscmp(args[i], L"--dialog") == 0)
        {
            dialog = true;
        }
        else if (std::wcscmp(args[i], L"--min") == 0 && more)
        {
            ++i;
            g_minimum.x = static_cast<LONG>(std::wcstol(args[i], nullptr, 10));
            if (const wchar_t* x = std::wcschr(args[i], L'x'); x != nullptr)
            {
                g_minimum.y = static_cast<LONG>(std::wcstol(x + 1, nullptr, 10));
            }
        }
        else if (std::wcscmp(args[i], L"--fullscreen-after") == 0 && more)
        {
            fullscreenAfter = static_cast<UINT>(std::wcstoul(args[++i], nullptr, 10));
        }
        else if (std::wcscmp(args[i], L"--tray-after") == 0 && more)
        {
            trayAfter = static_cast<UINT>(std::wcstoul(args[++i], nullptr, 10));
        }
        else if (std::wcscmp(args[i], L"--maximized") == 0)
        {
            maximized = true;
        }
        else if (args[i][0] != L'\0')
        {
            title = args[i];
        }
    }
    LocalFree(static_cast<void*>(args));

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClass;
    RegisterClassExW(&windowClass);
    const DWORD style = fixed ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) : WS_OVERLAPPEDWINDOW;
    HWND window = CreateWindowExW(0, kClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, 480, 360, nullptr,
                                  nullptr, instance, nullptr);
    ShowWindow(window, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    if (fullscreenAfter > 0)
    {
        SetTimer(window, kFullscreenTimer, fullscreenAfter, nullptr);
    }
    if (trayAfter > 0)
    {
        SetTimer(window, kTrayTimer, trayAfter, nullptr);
    }
    if (dialog)
    {
        const std::wstring dialogTitle = title + L" dialog";
        HWND owned = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, dialogTitle.c_str(),
                                     WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 260, 140,
                                     window, nullptr, instance, nullptr);
        ShowWindow(owned, SW_SHOWNORMAL);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
