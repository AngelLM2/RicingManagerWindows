#include "rmw/providers/Windows.hpp"

#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <span>

namespace rmw::providers
{
namespace
{

constexpr int kMaxTitle = 256;

std::atomic<void (*)()> g_changed{nullptr}; // WinEvent callbacks carry no context

void CALLBACK onEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD)
{
    if (object != OBJID_WINDOW || child != CHILDID_SELF || window == nullptr)
    {
        return;
    }
    // A destroyed window has no ancestor anymore; anything else must be top-level.
    if (event != EVENT_OBJECT_DESTROY && GetAncestor(window, GA_ROOT) != window)
    {
        return;
    }
    if (auto* changed = g_changed.load())
    {
        changed();
    }
}

} // namespace

bool isTaskbarWindow(void* handle)
{
    const auto window = static_cast<HWND>(handle);
    if (IsWindowVisible(window) == FALSE || GetAncestor(window, GA_ROOT) != window)
    {
        return false;
    }
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if ((extended & WS_EX_APPWINDOW) == 0 &&
        (GetWindow(window, GW_OWNER) != nullptr || (extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0))
    {
        return false;
    }
    // Cloaked: on another virtual desktop, or a suspended store app.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
    {
        return false;
    }
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process != GetCurrentProcessId() && GetWindowTextLengthW(window) > 0;
}

std::string windowTitle(void* window)
{
    std::array<wchar_t, kMaxTitle> text{};
    const int length = GetWindowTextW(static_cast<HWND>(window), text.data(), static_cast<int>(text.size()));
    return utf16To8Lossy(std::wstring_view(text.data(), static_cast<std::size_t>(std::max(0, length))));
}

std::string windowApp(void* window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(static_cast<HWND>(window), &processId);
    const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process)
    {
        return {};
    }
    std::array<wchar_t, MAX_PATH> path{};
    auto length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length) == FALSE)
    {
        return {};
    }
    return utf16To8Lossy(std::filesystem::path(std::wstring_view(path.data(), length)).filename().native());
}

std::vector<void*> taskbarWindows()
{
    std::vector<void*> windows;
    EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            if (isTaskbarWindow(window))
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands our pointer back
                reinterpret_cast<std::vector<void*>*>(list)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void activateWindow(void* handle)
{
    const auto window = static_cast<HWND>(handle);
    if (IsWindow(window) == FALSE)
    {
        return;
    }
    if (window == GetForegroundWindow() && IsIconic(window) == FALSE)
    {
        ShowWindowAsync(window, SW_MINIMIZE);
        return;
    }
    focusWindow(handle);
}

void focusWindow(void* handle)
{
    const auto window = static_cast<HWND>(handle);
    if (IsWindow(window) == FALSE)
    {
        return;
    }
    if (IsIconic(window) != FALSE)
    {
        ShowWindowAsync(window, SW_RESTORE);
    }
    // After a click on the bar Windows lets us hand the foreground over; if it still refuses
    // (the foreground lock), do what Alt+Tab does.
    if (SetForegroundWindow(window) == FALSE)
    {
        SwitchToThisWindow(window, TRUE);
    }
}

void minimizeWindow(void* handle)
{
    if (IsWindow(static_cast<HWND>(handle)) != FALSE)
    {
        ShowWindowAsync(static_cast<HWND>(handle), SW_MINIMIZE);
    }
}

void closeWindow(void* handle)
{
    if (IsWindow(static_cast<HWND>(handle)) != FALSE)
    {
        PostMessageW(static_cast<HWND>(handle), WM_CLOSE, 0, 0);
    }
}

IconPixels iconPixels(void* handle, int size)
{
    constexpr int kMaxSide = 64;
    auto* const icon = static_cast<HICON>(handle);
    if (icon == nullptr)
    {
        return {};
    }
    if (size <= 0)
    {
        ICONINFO info{};
        if (GetIconInfo(icon, &info) == FALSE)
        {
            return {};
        }
        const wil::unique_hbitmap color(info.hbmColor);
        const wil::unique_hbitmap mask(info.hbmMask);
        BITMAP bitmap{};
        // A monochrome icon has only a mask, twice as tall as wide: the width is the side.
        if (GetObjectW(color ? color.get() : mask.get(), sizeof(bitmap), &bitmap) == 0)
        {
            return {};
        }
        size = bitmap.bmWidth;
    }
    size = std::clamp(size, 1, kMaxSide);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = size;
    bitmapInfo.bmiHeader.biHeight = -size; // top-down
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    const wil::unique_hdc dc(CreateCompatibleDC(nullptr));
    void* colorBits = nullptr;
    void* maskBits = nullptr;
    const wil::unique_hbitmap color(
        CreateDIBSection(dc.get(), &bitmapInfo, DIB_RGB_COLORS, &colorBits, nullptr, 0));
    const wil::unique_hbitmap mask(
        CreateDIBSection(dc.get(), &bitmapInfo, DIB_RGB_COLORS, &maskBits, nullptr, 0));
    if (!dc || !color || !mask)
    {
        return {};
    }
    HGDIOBJ old = SelectObject(dc.get(), color.get());
    const bool drawn = DrawIconEx(dc.get(), 0, 0, icon, size, size, 0, nullptr, DI_NORMAL) != FALSE;
    SelectObject(dc.get(), mask.get());
    DrawIconEx(dc.get(), 0, 0, icon, size, size, 0, nullptr, DI_MASK); // white = transparent
    SelectObject(dc.get(), old);
    if (!drawn)
    {
        return {};
    }

    const auto pixelCount = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    const std::span pixels(static_cast<const std::uint32_t*>(colorBits), pixelCount);
    const std::span maskPixels(static_cast<const std::uint32_t*>(maskBits), pixelCount);
    // Old icons have no alpha channel (it stays 0): their mask says what is drawn.
    const bool alpha = std::ranges::any_of(pixels, [](std::uint32_t p) { return (p >> 24) != 0; });
    IconPixels out;
    out.width = size;
    out.height = size;
    out.rgba.resize(pixelCount * 4);
    for (std::size_t i = 0; i < pixelCount; ++i)
    {
        const std::uint32_t p = pixels[i];
        std::uint32_t a = p >> 24;
        if (!alpha)
        {
            a = (maskPixels[i] & 0x00FFFFFFU) != 0 ? 0U : 255U;
        }
        std::uint32_t r = (p >> 16) & 0xFFU;
        std::uint32_t g = (p >> 8) & 0xFFU;
        std::uint32_t b = p & 0xFFU;
        if (alpha && a != 0 && a != 255) // GDI wrote premultiplied color
        {
            r = std::min(255U, r * 255U / a);
            g = std::min(255U, g * 255U / a);
            b = std::min(255U, b * 255U / a);
        }
        out.rgba[(i * 4) + 0] = static_cast<std::uint8_t>(r);
        out.rgba[(i * 4) + 1] = static_cast<std::uint8_t>(g);
        out.rgba[(i * 4) + 2] = static_cast<std::uint8_t>(b);
        out.rgba[(i * 4) + 3] = static_cast<std::uint8_t>(a);
    }
    return out;
}

WindowEventHooks::WindowEventHooks(void (*changed)())
{
    g_changed = changed;
    const std::array<std::pair<DWORD, DWORD>, 5> ranges = {{
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    }};
    for (const auto& [first, last] : ranges)
    {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, &onEvent, 0, 0,
                                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
        {
            m_hooks.push_back(hook);
        }
    }
}

WindowEventHooks::~WindowEventHooks()
{
    for (void* hook : m_hooks)
    {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook));
    }
    g_changed = nullptr;
}

} // namespace rmw::providers
