// rmw_tray_test_app.exe <n> <version> <top|message> <log>
// A tray icon for the M5 system tests (tests/system/sandbox/tray-tests.ps1): adds icon <n> with
// NOTIFYICON_VERSION <version> (0, 3 or 4) from a top-level or a message-only window, adds it again
// on TaskbarCreated, and writes every callback it gets to <log>. The test drives it with messages
// to its window (class "RmwTrayTest<n>"):
//   WM_APP+10 new tip, +11 hide, +12 show, +13 delete, +14 add again, +15 quit,
//   +16 log where the tray says the icon is (Shell_NotifyIconGetRect).

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <format>
#include <fstream>
#include <string>

namespace
{

constexpr UINT kCallback = WM_APP + 1;
constexpr UINT kNewTip = WM_APP + 10;
constexpr UINT kHide = WM_APP + 11;
constexpr UINT kShow = WM_APP + 12;
constexpr UINT kDelete = WM_APP + 13;
constexpr UINT kAddAgain = WM_APP + 14;
constexpr UINT kQuit = WM_APP + 15;
constexpr UINT kAskRect = WM_APP + 16;

std::ofstream* g_log = nullptr; // wmain's
NOTIFYICONDATAW g_icon{};
UINT g_taskbarCreated = 0;
UINT g_version = 0;

void line(const std::string& text)
{
    if (g_log != nullptr)
    {
        *g_log << text << '\n';
        g_log->flush();
    }
}

void logRect()
{
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = g_icon.hWnd;
    identifier.uID = g_icon.uID;
    RECT rect{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &rect)))
    {
        line(std::format("rect ok {} {} {} {}", rect.left, rect.top, rect.right, rect.bottom));
    }
    else
    {
        line("rect fail");
    }
}

void add()
{
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &g_icon);
    if (g_version != 0)
    {
        g_icon.uVersion = g_version;
        Shell_NotifyIconW(NIM_SETVERSION, &g_icon);
    }
    line(std::format("add {}", added != FALSE ? "ok" : "failed"));
}

const char* eventName(UINT event)
{
    switch (event)
    {
    case WM_MOUSEMOVE:
        return "mousemove";
    case WM_LBUTTONDOWN:
        return "lbuttondown";
    case WM_LBUTTONUP:
        return "lbuttonup";
    case WM_LBUTTONDBLCLK:
        return "dblclk";
    case WM_RBUTTONDOWN:
        return "rbuttondown";
    case WM_RBUTTONUP:
        return "rbuttonup";
    case WM_MBUTTONDOWN:
        return "mbuttondown";
    case WM_MBUTTONUP:
        return "mbuttonup";
    case WM_CONTEXTMENU:
        return "contextmenu";
    case NIN_SELECT:
        return "select";
    case NIN_KEYSELECT:
        return "keyselect";
    case NIN_POPUPOPEN:
        return "popupopen";
    case NIN_POPUPCLOSE:
        return "popupclose";
    default:
        return "other";
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kCallback)
    {
        if (g_version >= 4)
        {
            // wParam: the anchor point; lParam: the event, and our id in the high word.
            line(std::format("event {} id={} at {},{}", eventName(LOWORD(lParam)), HIWORD(lParam),
                             static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))));
        }
        else
        {
            line(std::format("event {} id={}", eventName(static_cast<UINT>(lParam)), wParam));
        }
        const UINT event = g_version >= 4 ? LOWORD(lParam) : static_cast<UINT>(lParam);
        if (event == WM_CONTEXTMENU || event == NIN_SELECT || (g_version < 3 && event == WM_LBUTTONUP))
        {
            logRect(); // where an app would put its menu
        }
        return 0;
    }
    if (message == g_taskbarCreated && g_taskbarCreated != 0)
    {
        line("taskbarcreated");
        add();
        return 0;
    }
    switch (message)
    {
    case kNewTip:
        swprintf_s(g_icon.szTip, L"rmw tray %u changed", g_icon.uID);
        g_icon.uFlags = NIF_TIP | NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_icon);
        g_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        line("tip changed");
        return 0;
    case kHide:
    case kShow:
        g_icon.uFlags = NIF_STATE;
        g_icon.dwState = message == kHide ? NIS_HIDDEN : 0;
        g_icon.dwStateMask = NIS_HIDDEN;
        Shell_NotifyIconW(NIM_MODIFY, &g_icon);
        g_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        line(message == kHide ? "hidden" : "shown");
        return 0;
    case kDelete:
        Shell_NotifyIconW(NIM_DELETE, &g_icon);
        line("deleted");
        return 0;
    case kAddAgain:
        add();
        return 0;
    case kAskRect:
        logRect();
        return 0;
    case kQuit:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): the entry point
{
    if (argc < 5)
    {
        std::fputs("usage: rmw_tray_test_app <n> <version> <top|message> <log>\n", stderr);
        return 2;
    }
    const auto number = static_cast<UINT>(std::wcstoul(argv[1], nullptr, 10));
    g_version = static_cast<UINT>(std::wcstoul(argv[2], nullptr, 10));
    const bool messageOnly = std::wcscmp(argv[3], L"message") == 0;
    std::ofstream log(argv[4], std::ios::app);
    g_log = &log;

    const std::wstring className = std::format(L"RmwTrayTest{}", number);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className.c_str();
    RegisterClassExW(&windowClass);
    HWND window =
        CreateWindowExW(0, className.c_str(), L"", WS_OVERLAPPED, 0, 0, 0, 0,
                        messageOnly ? HWND_MESSAGE : nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(window, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    g_icon.cbSize = sizeof(g_icon);
    g_icon.hWnd = window;
    g_icon.uID = number;
    g_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_icon.uCallbackMessage = kCallback;
    g_icon.hIcon = LoadIconW(nullptr, number % 2 == 0 ? IDI_WARNING : IDI_INFORMATION);
    swprintf_s(g_icon.szTip, L"rmw tray %u", number);
    line(std::format("start version={} {}", g_version, messageOnly ? "message-only" : "top-level"));
    add();
    logRect();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_icon);
    line("quit");
    return 0;
}
