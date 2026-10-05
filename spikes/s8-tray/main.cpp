// Spike S8 — system tray in our own bar via the "spy window" technique (docs/09 §5, docs/04 S8).
// RUN ONLY INSIDE WINDOWS SANDBOX: it intercepts every app's tray-icon messages.
//
//   spike_s8.exe spy              Creates a topmost window of class "Shell_TrayWnd" so that
//                                 Shell_NotifyIcon finds it first, decodes WM_COPYDATA (dwData=1),
//                                 forwards everything to the real tray, broadcasts TaskbarCreated,
//                                 and forwards a click back to an app (callback message).
//   spike_s8.exe app <n> <file>   Test app: adds a tray icon (NOTIFYICON_VERSION_4), re-adds it on
//                                 TaskbarCreated, records callbacks received in <file>.

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <shellapi.h>
#include <wil/resource.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// Layout used by Shell_NotifyIcon inside WM_COPYDATA (handles are 32-bit on purpose).
#pragma pack(push, 4)
struct NotifyIconData32
{
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct ShellTrayData
{
    DWORD dwSignature; // 0x34753423
    DWORD dwMessage;   // NIM_ADD / NIM_MODIFY / NIM_DELETE / NIM_SETVERSION ...
    NotifyIconData32 nid;
};
#pragma pack(pop)

constexpr DWORD kTraySignature = 0x34753423;
constexpr UINT kAppCallback = WM_APP + 1;

struct TrayIcon
{
    HWND owner;
    UINT id;
    UINT callback;
    UINT version;
    std::wstring tip;
    HICON icon;
};

// ----------------------------------------------------------------------------------------------
// Spy
// ----------------------------------------------------------------------------------------------
std::map<std::pair<HWND, UINT>, TrayIcon> g_icons;
std::vector<std::string> g_events;
HWND g_spy = nullptr;
int g_forwardFailures = 0;

HWND realTray()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[64]{};
            GetClassNameW(hwnd, cls, 64);
            if (std::wstring_view(cls) == L"Shell_TrayWnd" && hwnd != g_spy)
            {
                *reinterpret_cast<HWND*>(param) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

LRESULT CALLBACK spyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA)
    {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        LRESULT forwarded = 0;
        // Forward first so the real tray behaves exactly as without us.
        if (const HWND tray = realTray())
        {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(tray, WM_COPYDATA, wp, lp, SMTO_ABORTIFHUNG, 2000, &result))
            {
                forwarded = static_cast<LRESULT>(result);
            }
            else
            {
                ++g_forwardFailures;
            }
        }
        if (cds && cds->dwData == 1 && cds->cbData >= sizeof(DWORD) * 2 + offsetof(NotifyIconData32, szTip))
        {
            const auto* data = static_cast<const ShellTrayData*>(cds->lpData);
            if (data->dwSignature == kTraySignature)
            {
                const auto owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(data->nid.hWnd));
                const auto key = std::make_pair(owner, data->nid.uID);
                const std::wstring tip(data->nid.szTip, wcsnlen(data->nid.szTip, 128));
                switch (data->dwMessage)
                {
                case NIM_ADD:
                case NIM_MODIFY: {
                    auto& icon = g_icons[key];
                    icon.owner = owner;
                    icon.id = data->nid.uID;
                    if (data->nid.uFlags & NIF_MESSAGE)
                    {
                        icon.callback = data->nid.uCallbackMessage;
                    }
                    if (data->nid.uFlags & NIF_TIP)
                    {
                        icon.tip = tip;
                    }
                    if (data->nid.uFlags & NIF_ICON)
                    {
                        icon.icon = reinterpret_cast<HICON>(static_cast<uintptr_t>(data->nid.hIcon));
                    }
                    break;
                }
                case NIM_SETVERSION:
                    g_icons[key].version = data->nid.uVersion;
                    break;
                case NIM_DELETE:
                    g_icons.erase(key);
                    break;
                default:
                    break;
                }
                g_events.push_back(std::format("msg={} tip='{}'", data->dwMessage, rmw::utf16To8Lossy(tip)));
            }
        }
        return forwarded ? forwarded : TRUE;
    }
    // Anything else (position queries, etc.) goes to the real tray unchanged.
    if (const HWND tray = realTray(); tray && msg >= WM_USER)
    {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(tray, msg, wp, lp, SMTO_ABORTIFHUNG, 1000, &result);
        return static_cast<LRESULT>(result);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void pump(int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end)
    {
        if (g_spy)
        {
            SetWindowPos(g_spy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
}

PROCESS_INFORMATION startApp(int n, const fs::path& log)
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::format(LR"("{}" app {} "{}")", self, n, log.wstring());
    STARTUPINFOW si{.cb = sizeof(si)};
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    return pi;
}

int runSpy()
{
    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
    {
        Sleep(1000);
    }
    Sleep(4000);

    const fs::path dir = fs::temp_directory_path() / "rmw-s8";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::string> results;
    const auto result = [&](const std::string& name, bool ok, const std::string& detail) {
        std::string safe;
        for (char c : detail)
        {
            safe += (c == '"' || c == '\\') ? '/' : c;
        }
        results.push_back(std::format("{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", name,
                                      ok ? "true" : "false", safe));
    };

    // App 1 registers BEFORE the spy exists (must be recovered through TaskbarCreated).
    const auto app1 = startApp(1, dir / "app1.log");
    Sleep(2000);

    WNDCLASSEXW wc{.cbSize = sizeof(wc),
                   .lpfnWndProc = spyProc,
                   .hInstance = GetModuleHandleW(nullptr),
                   .lpszClassName = L"Shell_TrayWnd"};
    RegisterClassExW(&wc);
    g_spy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"Shell_TrayWnd", L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, wc.hInstance, nullptr);
    ChangeWindowMessageFilterEx(g_spy, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    pump(500);
    const bool spyFirst = FindWindowW(L"Shell_TrayWnd", nullptr) == g_spy;
    result("spy:found-first-by-FindWindow", spyFirst, std::format("realTray={}", realTray() != nullptr));

    // Ask every app to re-register its icons.
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    // App 2 registers WHILE the spy exists.
    const auto app2 = startApp(2, dir / "app2.log");
    pump(4000);

    int seenApps = 0;
    std::string tips;
    for (const auto& [key, icon] : g_icons)
    {
        tips += rmw::utf16To8Lossy(icon.tip) + std::format("(v{}) ", icon.version);
        if (icon.tip.starts_with(L"rmw test"))
        {
            ++seenApps;
        }
    }
    result("spy:icons-captured", seenApps == 2,
           std::format("{} icon(s): {} events={}", g_icons.size(), tips, g_events.size()));
    result("spy:forwarded-to-real-tray", g_forwardFailures == 0,
           std::format("forward failures={}", g_forwardFailures));

    // Simulate a left click and a context menu on app 2's icon (NOTIFYICON_VERSION_4 semantics).
    for (const auto& [key, icon] : g_icons)
    {
        if (icon.tip == L"rmw test 2")
        {
            const WPARAM where = MAKEWPARAM(100, 100);
            PostMessageW(icon.owner, icon.callback, where, MAKELPARAM(WM_LBUTTONUP, icon.id));
            PostMessageW(icon.owner, icon.callback, where, MAKELPARAM(WM_CONTEXTMENU, icon.id));
        }
    }
    pump(1500);

    // Remove the spy and let apps re-register with the real tray.
    DestroyWindow(g_spy);
    g_spy = nullptr;
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    pump(3000);

    // Let the apps finish and write their logs.
    for (const auto& pi : {app1, app2})
    {
        PostThreadMessageW(pi.dwThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    const auto readLog = [](const fs::path& p) {
        std::ifstream in(p);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string log1 = readLog(dir / "app1.log");
    const std::string log2 = readLog(dir / "app2.log");
    result("app:click-forwarded",
           log2.find("click") != std::string::npos && log2.find("menu") != std::string::npos, log2);
    result("app:icon-in-real-tray-while-spy",
           log1.find("rect-ok") != std::string::npos || log2.find("rect-ok") != std::string::npos,
           "app1: " + log1 + " | app2: " + log2);

    std::string json = "{\"results\":[";
    for (size_t i = 0; i < results.size(); ++i)
    {
        json += (i ? "," : "") + results[i];
    }
    json += "]}";
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    return 0;
}

// ----------------------------------------------------------------------------------------------
// Test app
// ----------------------------------------------------------------------------------------------
std::wofstream* g_log = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;

void addIcon()
{
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    NOTIFYICONIDENTIFIER ident{.cbSize = sizeof(ident), .hWnd = g_nid.hWnd, .uID = g_nid.uID};
    RECT rect{};
    *g_log << (SUCCEEDED(Shell_NotifyIconGetRect(&ident, &rect)) ? L"rect-ok " : L"rect-fail ");
    g_log->flush();
}

LRESULT CALLBACK appProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kAppCallback)
    {
        const UINT event = LOWORD(lp);
        if (event == WM_LBUTTONUP)
        {
            *g_log << L"click ";
        }
        else if (event == WM_CONTEXTMENU)
        {
            *g_log << L"menu ";
        }
        g_log->flush();
        return 0;
    }
    if (msg == g_taskbarCreated && g_taskbarCreated != 0)
    {
        *g_log << L"taskbarcreated ";
        addIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runApp(int n, const fs::path& logPath)
{
    std::wofstream log(logPath);
    g_log = &log;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{.cbSize = sizeof(wc),
                   .lpfnWndProc = appProc,
                   .hInstance = GetModuleHandleW(nullptr),
                   .lpszClassName = L"RmwS8TestApp"};
    RegisterClassExW(&wc);
    const HWND hwnd = CreateWindowExW(0, L"RmwS8TestApp", L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                      wc.hInstance, nullptr);
    ChangeWindowMessageFilterEx(hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = static_cast<UINT>(n);
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = kAppCallback;
    g_nid.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcscpy_s(g_nid.szTip, std::format(L"rmw test {}", n).c_str());
    addIcon();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring_view mode = argc > 1 ? argv[1] : L"spy";
    if (mode == L"app" && argc > 3)
    {
        return runApp(_wtoi(argv[2]), argv[3]);
    }
    return runSpy();
}
