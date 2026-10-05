// Spike S3 — native taskbar tweaks without injection (docs/04, docs/09 §5).
// RUN ONLY INSIDE WINDOWS SANDBOX: it changes real taskbar settings.
//
// Checks, on the live Explorer:
//  1. TaskbarAl (alignment), ShowTaskViewButton, SearchboxTaskbarMode apply live after a
//     "TraySettings" broadcast — verified through UI Automation on the taskbar.
//  2. Auto-hide via SHAppBarMessage(ABM_SETSTATE).
//  3. Hiding Shell_TrayWnd (SW_HIDE) and restoring it.
//  4. Whether the Start menu still opens (Win key) while the taskbar is hidden.
// Prints one JSON object.

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>
// WIN32_LEAN_AND_MEAN drops the OLE headers that UI Automation needs.
#include <dwmapi.h>
#include <shellapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <ole2.h>
#include <uiautomation.h>

#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::vector<std::string> g_results;

void result(const std::string& name, bool ok, const std::string& detail = {})
{
    std::string safe;
    for (char c : detail)
    {
        safe += (c == '"' || c == '\\') ? '\'' : c;
    }
    g_results.push_back(
        std::format("{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", name, ok ? "true" : "false", safe));
}

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool setDword(const wchar_t* subKey, const wchar_t* name, DWORD value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, subKey, name, REG_DWORD, &value, sizeof(value)) ==
           ERROR_SUCCESS;
}

void broadcastTraySettings()
{
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"),
                        SMTO_ABORTIFHUNG, 2000, &ignored);
}

constexpr auto kAdvanced = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr auto kSearch = L"Software\\Microsoft\\Windows\\CurrentVersion\\Search";

struct TaskbarProbe
{
    bool found = false;
    RECT taskbar{};
    bool hasStart = false;
    RECT start{};
    bool hasTaskView = false;
    std::string searchKind; // "none", or the automation id found
    std::string ids;        // diagnostics
};

TaskbarProbe probeTaskbar(IUIAutomation* uia)
{
    TaskbarProbe probe;
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray)
    {
        return probe;
    }
    probe.found = true;
    GetWindowRect(tray, &probe.taskbar);

    wil::com_ptr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(tray, &root)) || !root)
    {
        return probe;
    }
    wil::com_ptr<IUIAutomationCondition> all;
    uia->CreateTrueCondition(&all);
    wil::com_ptr<IUIAutomationElementArray> elements;
    if (FAILED(root->FindAll(TreeScope_Descendants, all.get(), &elements)) || !elements)
    {
        return probe;
    }
    int count = 0;
    elements->get_Length(&count);
    probe.searchKind = "none";
    for (int i = 0; i < count; ++i)
    {
        wil::com_ptr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(i, &element)))
        {
            continue;
        }
        wil::unique_bstr id;
        element->get_CurrentAutomationId(&id);
        BOOL offscreen = FALSE;
        element->get_CurrentIsOffscreen(&offscreen);
        if (!id || SysStringLen(id.get()) == 0 || offscreen)
        {
            continue;
        }
        const std::wstring_view wid(id.get(), SysStringLen(id.get()));
        if (probe.ids.size() < 600)
        {
            probe.ids += rmw::utf16To8Lossy(wid) + " ";
        }
        if (wid == L"StartButton")
        {
            probe.hasStart = true;
            element->get_CurrentBoundingRectangle(&probe.start);
        }
        else if (wid == L"TaskViewButton")
        {
            probe.hasTaskView = true;
        }
        else if (wid.find(L"Search") != std::wstring_view::npos && probe.searchKind == "none")
        {
            probe.searchKind = rmw::utf16To8Lossy(wid);
        }
    }
    return probe;
}

bool startIsLeft(const TaskbarProbe& p)
{
    return p.hasStart && (p.start.left - p.taskbar.left) < 120;
}

bool startMenuVisible()
{
    bool visible = false;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[128]{};
            GetClassNameW(hwnd, cls, 128);
            if (std::wstring_view(cls) != L"Windows.UI.Core.CoreWindow")
            {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (!process || !QueryFullProcessImageNameW(process.get(), 0, path, &size))
            {
                return TRUE;
            }
            if (std::wstring_view(path).find(L"StartMenuExperienceHost") == std::wstring_view::npos)
            {
                return TRUE;
            }
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (IsWindowVisible(hwnd) && !cloaked)
            {
                *reinterpret_cast<bool*>(param) = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&visible));
    return visible;
}

std::string foregroundInfo()
{
    const HWND fg = GetForegroundWindow();
    wchar_t cls[128]{};
    GetClassNameW(fg, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (process)
    {
        QueryFullProcessImageNameW(process.get(), 0, path, &size);
    }
    std::wstring_view exe(path);
    if (const auto slash = exe.find_last_of(L'\\'); slash != std::wstring_view::npos)
    {
        exe.remove_prefix(slash + 1);
    }
    wchar_t title[128]{};
    GetWindowTextW(fg, title, 128);
    return std::format("fg={} '{}' ({})", rmw::utf16To8Lossy(cls), rmw::utf16To8Lossy(title),
                       rmw::utf16To8Lossy(exe));
}

bool startSurfaceInForeground()
{
    const std::string fg = foregroundInfo();
    return fg.find("CoreWindow") != std::string::npos &&
           (fg.find("SearchHost") != std::string::npos ||
            fg.find("StartMenuExperienceHost") != std::string::npos);
}

void restartExplorer()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
    wil::unique_handle explorer(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
    if (explorer)
    {
        TerminateProcess(explorer.get(), 1);
        WaitForSingleObject(explorer.get(), 10000);
    }
    sleepMs(2000);
    // Winlogon restarts the shell automatically; start it ourselves if it did not.
    if (!FindWindowW(L"Shell_TrayWnd", nullptr))
    {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }
    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
    {
        sleepMs(500);
    }
    sleepMs(6000);
}

void openStartViaSysCommand()
{
    PostMessageW(FindWindowW(L"Shell_TrayWnd", nullptr), WM_SYSCOMMAND, SC_TASKLIST, 0);
}

void pressWinKey()
{
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_LWIN;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

} // namespace

int wmain()
{
    const auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    auto uia = wil::CoCreateInstance<IUIAutomation>(CLSID_CUIAutomation, CLSCTX_INPROC_SERVER);

    // Wait for Explorer's taskbar after logon.
    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
    {
        sleepMs(1000);
    }
    sleepMs(5000);

    const auto initial = probeTaskbar(uia.get());
    result("taskbar:found", initial.found, std::format("ids: {}", initial.ids));

    // 1a. Alignment left, then center. Variant A: broadcast + longer wait.
    setDword(kAdvanced, L"TaskbarAl", 0);
    broadcastTraySettings();
    sleepMs(6000);
    const auto leftA = probeTaskbar(uia.get());
    // Variant B: notify the taskbar window directly.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(FindWindowW(L"Shell_TrayWnd", nullptr), WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"TraySettings"), SMTO_ABORTIFHUNG, 2000, &ignored);
    sleepMs(4000);
    const auto leftB = probeTaskbar(uia.get());
    // Variant C: generic WM_SETTINGCHANGE with no section name.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored);
    sleepMs(4000);
    const auto leftC = probeTaskbar(uia.get());
    result("tweak:TaskbarAl:live", startIsLeft(leftA) || startIsLeft(leftB) || startIsLeft(leftC),
           std::format("start.x A={} B={} C={} taskbar.x={}", leftA.start.left, leftB.start.left,
                       leftC.start.left, leftA.taskbar.left));
    // Variant D: value honored after an Explorer restart?
    restartExplorer();
    const auto leftD = probeTaskbar(uia.get());
    setDword(kAdvanced, L"TaskbarAl", 1);
    restartExplorer();
    const auto center = probeTaskbar(uia.get());
    result("tweak:TaskbarAl:after-explorer-restart", startIsLeft(leftD) && !startIsLeft(center),
           std::format("start.x left={} center={}", leftD.start.left, center.start.left));

    // 1b. Task View button off, then on.
    setDword(kAdvanced, L"ShowTaskViewButton", 0);
    broadcastTraySettings();
    sleepMs(2500);
    const auto noTaskView = probeTaskbar(uia.get());
    setDword(kAdvanced, L"ShowTaskViewButton", 1);
    broadcastTraySettings();
    sleepMs(2500);
    const auto withTaskView = probeTaskbar(uia.get());
    result("tweak:ShowTaskViewButton", !noTaskView.hasTaskView && withTaskView.hasTaskView,
           std::format("off={} on={}", noTaskView.hasTaskView, withTaskView.hasTaskView));

    // 1c. Search: hidden (0) vs icon (1).
    setDword(kSearch, L"SearchboxTaskbarMode", 0);
    broadcastTraySettings();
    sleepMs(2500);
    const auto noSearch = probeTaskbar(uia.get());
    setDword(kSearch, L"SearchboxTaskbarMode", 1);
    broadcastTraySettings();
    sleepMs(2500);
    const auto searchIcon = probeTaskbar(uia.get());
    result("tweak:SearchboxTaskbarMode", noSearch.searchKind == "none" && searchIcon.searchKind != "none",
           std::format("hidden={} icon={}", noSearch.searchKind, searchIcon.searchKind));

    // 2. Auto-hide through the AppBar API.
    APPBARDATA abd{.cbSize = sizeof(APPBARDATA), .hWnd = FindWindowW(L"Shell_TrayWnd", nullptr)};
    abd.lParam = ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    sleepMs(1500);
    const auto stateOn = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    abd.lParam = ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    sleepMs(1500);
    const auto stateOff = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    result("appbar:autohide", (stateOn & ABS_AUTOHIDE) != 0 && (stateOff & ABS_AUTOHIDE) == 0,
           std::format("on=0x{:X} off=0x{:X}", stateOn, stateOff));

    // 5 (baseline first). Start menu detection with the taskbar visible.
    openStartViaSysCommand();
    sleepMs(2500);
    const bool startBaselineSys = startMenuVisible() || startSurfaceInForeground();
    const std::string fgBaselineSys = foregroundInfo();
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    sleepMs(1500);
    pressWinKey();
    sleepMs(2500);
    const bool startBaselineKey = startMenuVisible() || startSurfaceInForeground();
    const std::string fgBaselineKey = foregroundInfo();
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    sleepMs(1500);
    result("baseline:start-menu", startBaselineSys || startBaselineKey,
           std::format("syscommand={} [{}] winkey={} [{}]", startBaselineSys, fgBaselineSys, startBaselineKey,
                       fgBaselineKey));

    // 3 + 4. Hide the native taskbar (auto-hide first so the work area is released, then
    // SW_HIDE, re-hiding for a few seconds like Seelen UI does), try the Start menu, restore.
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    abd.lParam = ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    sleepMs(1000);
    int reshown = 0;
    for (int i = 0; i < 60; ++i) // ~3 s
    {
        if (IsWindowVisible(tray))
        {
            ShowWindow(tray, SW_HIDE);
            ++reshown;
        }
        sleepMs(50);
    }
    sleepMs(1500);
    const bool hidden = !IsWindowVisible(tray);
    result("native:hide", hidden, std::format("hide calls={}", reshown));

    openStartViaSysCommand();
    sleepMs(2500);
    bool startOpened = startMenuVisible() || startSurfaceInForeground();
    std::string how = "syscommand";
    if (!startOpened)
    {
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        sleepMs(1000);
        pressWinKey();
        sleepMs(2500);
        startOpened = startMenuVisible() || startSurfaceInForeground();
        how = "winkey";
    }
    const bool stillHidden = !IsWindowVisible(tray);
    result("native:start-menu-while-hidden", startOpened,
           std::format("via={} taskbar-still-hidden={} [{}]", how, stillHidden, foregroundInfo()));
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    sleepMs(1500);

    ShowWindow(tray, SW_SHOW);
    abd.lParam = ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    sleepMs(1500);
    result("native:restore", IsWindowVisible(tray) != FALSE);

    std::string json = "{\"results\":[";
    for (std::size_t i = 0; i < g_results.size(); ++i)
    {
        json += (i ? "," : "") + g_results[i];
    }
    json += "]}";
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    return 0;
}
