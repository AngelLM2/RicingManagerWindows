#include "WindowOps.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace rmw::tilerapp
{
namespace
{

constexpr DWORD kBorderColorAttribute = 34; // DWMWA_BORDER_COLOR (Windows 11)
constexpr std::uint32_t kDefaultColor = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
constexpr int kMinimumSize = 48;                   // smaller "windows" are helpers nobody looks at
constexpr std::uint64_t kMaxOriginalsBytes = std::uint64_t{4} << 20;

// The shell's own surfaces: never tiled.
constexpr auto kShellClasses = std::to_array<std::wstring_view>({
    L"Progman",
    L"WorkerW",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"Windows.UI.Core.CoreWindow",
    L"XamlExplorerHostIslandWindow",
    L"TopLevelWindowForOverflowXamlIsland",
    L"MultitaskingViewFrame",
    L"ForegroundStaging",
    L"Shell_InputSwitchTopLevelWindow",
    L"NotifyIconOverflowWindow",
    L"TaskListThumbnailWnd",
    L"#32768", // menus
    L"#32770", // dialogs without an owner (Run, message boxes)
});

HWND hwnd(Window window) noexcept
{
    return static_cast<HWND>(window);
}

std::wstring classOf(HWND window)
{
    std::array<wchar_t, 256> name{};
    const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    return {name.data(), static_cast<std::size_t>(std::max(length, 0))};
}

bool cloaked(HWND window) noexcept
{
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

std::wstring imageOf(DWORD process)
{
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (handle == nullptr)
    {
        return {};
    }
    std::array<wchar_t, 1024> path{};
    auto length = static_cast<DWORD>(path.size());
    const BOOL found = QueryFullProcessImageNameW(handle, 0, path.data(), &length);
    CloseHandle(handle);
    return found != FALSE ? std::wstring(path.data(), length) : std::wstring();
}

// The integrity level of a token (SECURITY_MANDATORY_*_RID), or nullopt when it cannot be read.
std::optional<DWORD> integrityOf(HANDLE token) noexcept
{
    std::array<std::byte, 128> buffer{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &size) ==
        FALSE)
    {
        return std::nullopt;
    }
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count == 0 ? std::nullopt : std::optional(*GetSidSubAuthority(label->Label.Sid, count - 1));
}

DWORD ourIntegrity() noexcept
{
    static const DWORD level = [] {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        {
            return static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID);
        }
        const auto found = integrityOf(token);
        CloseHandle(token);
        return found.value_or(SECURITY_MANDATORY_MEDIUM_RID);
    }();
    return level;
}

const std::filesystem::path& ourFolder()
{
    static const std::filesystem::path folder = [] {
        auto self = platform::currentExecutable();
        return self ? self->parent_path() : std::filesystem::path();
    }();
    return folder;
}

void sendMaskKey() noexcept
{
    std::array<INPUT, 2> inputs{};
    for (auto& input : inputs)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0xE8; // unassigned: nothing happens anywhere
        input.ki.dwExtraInfo = kOurKeys;
    }
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

tiling::Rect rectOf(const RECT& r) noexcept
{
    return {r.left, r.top, r.right, r.bottom};
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/, LPARAM context)
{
    auto& out = *reinterpret_cast<std::vector<tiling::Monitor>*>(context); // NOLINT(performance-no-int-to-ptr)
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE)
    {
        return TRUE;
    }
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        dpiX = 96;
    }
    tiling::Monitor entry{utf16To8Lossy(info.szDevice), rectOf(info.rcWork), rectOf(info.rcMonitor), dpiX / 96.0};
    if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0)
    {
        out.insert(out.begin(), std::move(entry));
    }
    else
    {
        out.push_back(std::move(entry));
    }
    return TRUE;
}

} // namespace

bool isAppWindow(Window window) noexcept
{
    HWND w = hwnd(window);
    if (IsWindow(w) == FALSE || IsWindowVisible(w) == FALSE || IsIconic(w) != FALSE ||
        GetAncestor(w, GA_ROOT) != w || GetWindow(w, GW_OWNER) != nullptr)
    {
        return false;
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(w, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(w, GWL_EXSTYLE));
    if ((style & WS_CHILD) != 0 || (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0)
    {
        return false;
    }
    // A frame: a caption, or at least a resizable edge (apps that draw their own title bar).
    if ((style & WS_CAPTION) != WS_CAPTION && (style & WS_THICKFRAME) == 0)
    {
        return false;
    }
    if (cloaked(w))
    {
        return false;
    }
    try
    {
        const std::wstring name = classOf(w);
        if (std::ranges::find(kShellClasses, std::wstring_view(name)) != kShellClasses.end())
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    const tiling::Rect frame = frameOf(window);
    return frame.width() >= kMinimumSize && frame.height() >= kMinimumSize;
}

bool isResizable(Window window) noexcept
{
    return (static_cast<DWORD>(GetWindowLongPtrW(hwnd(window), GWL_STYLE)) & WS_THICKFRAME) != 0;
}

tiling::WindowFacts factsOf(Window window)
{
    tiling::WindowFacts facts;
    HWND w = hwnd(window);
    facts.windowClass = utf16To8Lossy(classOf(w));
    // Another process's window: Windows answers from the caption it keeps, without asking the app.
    std::array<wchar_t, 512> title{};
    const int length = GetWindowTextW(w, title.data(), static_cast<int>(title.size()));
    facts.title = utf16To8Lossy(std::wstring_view(title.data(), static_cast<std::size_t>(std::max(length, 0))));
    const std::wstring image = imageOf(processOf(window));
    facts.app = utf16To8Lossy(std::filesystem::path(image).filename().native());
    return facts;
}

std::uint32_t processOf(Window window) noexcept
{
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd(window), &process);
    return process;
}

bool canMove(Window window) noexcept
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processOf(window));
    if (process == nullptr)
    {
        return false;
    }
    HANDLE token = nullptr;
    std::optional<DWORD> level;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE)
    {
        level = integrityOf(token);
        CloseHandle(token);
    }
    CloseHandle(process);
    // A token we may not even read belongs to a process above us.
    return level.has_value() && *level <= ourIntegrity();
}

bool runsAsAdmin() noexcept
{
    return ourIntegrity() >= SECURITY_MANDATORY_HIGH_RID;
}

bool isOurs(Window window) noexcept
{
    try
    {
        const std::filesystem::path image(imageOf(processOf(window)));
        if (image.empty() || ourFolder().empty() || image.parent_path() != ourFolder())
        {
            return false;
        }
        std::wstring name = image.filename().native();
        std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return name.starts_with(L"rmw-") && name != L"rmw-settings.exe";
    }
    catch (...)
    {
        return false;
    }
}

tiling::Rect frameOf(Window window) noexcept
{
    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd(window), DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
    {
        return rectOf(frame);
    }
    GetWindowRect(hwnd(window), &frame);
    return rectOf(frame);
}

void place(Window window, const tiling::Rect& frame) noexcept
{
    HWND w = hwnd(window);
    RECT outer{};
    if (GetWindowRect(w, &outer) == FALSE)
    {
        return;
    }
    // The invisible resize borders around the visible frame stay outside the slot.
    const tiling::Rect visible = frameOf(window);
    const int left = frame.left - (visible.left - outer.left);
    const int top = frame.top - (visible.top - outer.top);
    const int right = frame.right + (outer.right - visible.right);
    const int bottom = frame.bottom + (outer.bottom - visible.bottom);
    SetWindowPos(w, nullptr, left, top, right - left, bottom - top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

Insets insetsOf(Window window) noexcept
{
    RECT outer{};
    if (GetWindowRect(hwnd(window), &outer) == FALSE)
    {
        return {};
    }
    const tiling::Rect visible = frameOf(window);
    return {visible.left - outer.left, visible.top - outer.top, outer.right - visible.right,
            outer.bottom - visible.bottom};
}

void showInPlace(Window window, const tiling::Rect& frame, const Insets& insets) noexcept
{
    const int left = frame.left - insets.left;
    const int top = frame.top - insets.top;
    SetWindowPos(hwnd(window), HWND_TOP, left, top, frame.right + insets.right - left,
                 frame.bottom + insets.bottom - top, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void hide(Window window) noexcept
{
    ShowWindowAsync(hwnd(window), SW_HIDE);
}

void show(Window window) noexcept
{
    ShowWindowAsync(hwnd(window), SW_SHOWNA);
}

void setBorder(Window window, std::optional<std::uint32_t> colorref) noexcept
{
    const COLORREF color = colorref.value_or(kDefaultColor);
    DwmSetWindowAttribute(hwnd(window), kBorderColorAttribute, &color, sizeof(color));
}

void focus(Window window) noexcept
{
    sendMaskKey();
    SetForegroundWindow(hwnd(window));
}

Window foreground() noexcept
{
    return GetForegroundWindow();
}

std::vector<tiling::Monitor> monitorsNow()
{
    std::vector<tiling::Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

std::size_t monitorIndex(std::span<const tiling::Monitor> monitors, Window window) noexcept
{
    POINT at{};
    if (window != nullptr)
    {
        const tiling::Rect frame = frameOf(window);
        at = {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
    }
    else
    {
        GetCursorPos(&at);
    }
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (monitors[i].bounds.contains(at.x, at.y))
        {
            return i;
        }
    }
    // Off every monitor (half outside): the nearest Windows says.
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &info) != FALSE)
    {
        const tiling::Rect bounds = rectOf(info.rcMonitor);
        for (std::size_t i = 0; i < monitors.size(); ++i)
        {
            if (monitors[i].bounds == bounds)
            {
                return i;
            }
        }
    }
    return 0;
}

tiling::Original originalOf(Window window) noexcept
{
    tiling::Original original;
    original.window = idOf(window);
    original.process = processOf(window);
    RECT rect{};
    GetWindowRect(hwnd(window), &rect);
    original.rect = rectOf(rect);
    original.maximized = IsZoomed(hwnd(window)) != FALSE;
    return original;
}

std::vector<tiling::Original> putBack(std::span<const tiling::Original> windows)
{
    std::vector<tiling::Original> untouchable;
    for (const auto& original : windows)
    {
        HWND w = hwnd(windowOf(original.window));
        if (IsWindow(w) == FALSE || processOf(w) != original.process)
        {
            continue;
        }
        if (!canMove(w))
        {
            untouchable.push_back(original);
            continue;
        }
        if (original.hidden)
        {
            ShowWindowAsync(w, SW_SHOWNA);
        }
        if (original.placed && IsIconic(w) == FALSE)
        {
            const tiling::Rect& r = original.rect;
            SetWindowPos(w, nullptr, r.left, r.top, r.width(), r.height(),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
            if (original.maximized)
            {
                ShowWindowAsync(w, SW_MAXIMIZE);
            }
        }
        setBorder(w, std::nullopt);
    }
    return untouchable;
}

bool putBackFromFile(const std::filesystem::path& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
        return false;
    }
    auto bytes = platform::readFileLimited(file, kMaxOriginalsBytes);
    if (bytes)
    {
        const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (auto windows = tiling::parseOriginals(text))
        {
            const auto left = putBack(*windows);
            log::info("tiler: {} window(s) put back from the last run", windows->size() - left.size());
            if (!left.empty())
            {
                // An admin app's windows: an elevated tiler puts them back when it starts.
                log::info("tiler: {} window(s) of apps running as administrator wait for advanced mode",
                          left.size());
                const std::string kept = tiling::toJson(left);
                const auto* data = reinterpret_cast<const std::byte*>(kept.data());
                if (platform::writeFileAtomic(file, std::span(data, kept.size())))
                {
                    return true;
                }
            }
        }
        else
        {
            log::warn("tiler: {}", windows.error().toString());
        }
    }
    std::filesystem::remove(file, ec);
    return true;
}

} // namespace rmw::tilerapp
