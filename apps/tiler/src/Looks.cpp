#include "Looks.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace rmw::tilerapp
{
namespace
{

// Windows 11's attributes (dwmapi.h names them in newer SDKs; the numbers are what DWM reads).
constexpr DWORD kDarkMode = 20;     // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCorners = 33;      // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColor = 34;  // DWMWA_BORDER_COLOR
constexpr DWORD kCaptionColor = 35; // DWMWA_CAPTION_COLOR
constexpr DWORD kTextColor = 36;    // DWMWA_TEXT_COLOR
constexpr DWORD kBackdrop = 38;     // DWMWA_SYSTEMBACKDROP_TYPE
constexpr COLORREF kDefaultColor = 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
constexpr std::size_t kMaxPainted = 4096;

HWND hwnd(Window window) noexcept
{
    return static_cast<HWND>(window);
}

template <typename T>
void setAttribute(Window window, DWORD attribute, T value) noexcept
{
    DwmSetWindowAttribute(hwnd(window), attribute, &value, sizeof(value));
}

DWORD cornersValue(domain::WindowCorners corners) noexcept
{
    switch (corners)
    {
    case domain::WindowCorners::Square:
        return 1; // DWMWCP_DONOTROUND
    case domain::WindowCorners::Round:
        return 2; // DWMWCP_ROUND
    case domain::WindowCorners::Small:
        return 3; // DWMWCP_ROUNDSMALL
    }
    return 0;
}

DWORD backdropValue(domain::WindowBackdrop backdrop) noexcept
{
    switch (backdrop)
    {
    case domain::WindowBackdrop::None:
        return 1; // DWMSBT_NONE
    case domain::WindowBackdrop::Mica:
        return 2; // DWMSBT_MAINWINDOW
    case domain::WindowBackdrop::Acrylic:
        return 3; // DWMSBT_TRANSIENTWINDOW
    case domain::WindowBackdrop::Tabbed:
        return 4; // DWMSBT_TABBEDWINDOW
    }
    return 0;
}

COLORREF colorref(const domain::Color& color) noexcept
{
    return RGB(color.r, color.g, color.b);
}

// Apps' dark mode, as Settings sets it.
bool appsDark() noexcept
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

bool leftOut(const tiling::WindowLook& look, Window window)
{
    if (look.exclude.empty())
    {
        return false;
    }
    std::string app = factsOf(window).app;
    std::ranges::transform(app, app.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return std::ranges::find(look.exclude, app) != look.exclude.end();
}

BOOL CALLBACK collect(HWND window, LPARAM list)
{
    reinterpret_cast<std::vector<HWND>*>(list)->push_back(window); // NOLINT(performance-no-int-to-ptr)
    return TRUE;
}

} // namespace

Looks::~Looks()
{
    reset();
}

void Looks::set(const std::optional<tiling::WindowLook>& look)
{
    if (look == m_look)
    {
        return;
    }
    reset();
    m_look = look;
    if (!m_look)
    {
        return;
    }
    m_systemDark = appsDark();
    std::vector<HWND> windows;
    EnumWindows(&collect, reinterpret_cast<LPARAM>(&windows));
    for (HWND window : windows)
    {
        paint(window);
    }
    log::info("tiler: the rice's look on {} window(s)", m_painted.size());
}

void Looks::paint(Window window, bool again)
{
    if (!m_look || (!again && m_painted.contains(window)) || !isAppWindow(window) || isOurs(window) ||
        leftOut(*m_look, window))
    {
        return;
    }
    if (m_painted.size() >= kMaxPainted && !m_painted.contains(window))
    {
        // Handles of windows long gone: forget the ones Windows no longer knows.
        std::erase_if(m_painted, [](Window w) { return IsWindow(hwnd(w)) == FALSE; });
    }
    const tiling::WindowLook& look = *m_look;
    if (look.dark)
    {
        setAttribute<BOOL>(window, kDarkMode, *look.dark ? TRUE : FALSE);
    }
    if (look.corners)
    {
        setAttribute<DWORD>(window, kCorners, cornersValue(*look.corners));
    }
    if (const auto color = border())
    {
        setAttribute<COLORREF>(window, kBorderColor, *color);
    }
    if (look.caption)
    {
        setAttribute<COLORREF>(window, kCaptionColor, colorref(*look.caption));
    }
    if (look.text)
    {
        setAttribute<COLORREF>(window, kTextColor, colorref(*look.text));
    }
    if (look.backdrop)
    {
        setAttribute<DWORD>(window, kBackdrop, backdropValue(*look.backdrop));
    }
    m_painted.insert(window);
}

void Looks::forget(Window window) noexcept
{
    m_painted.erase(window);
}

void Looks::reset() noexcept
{
    if (!m_look)
    {
        return;
    }
    const tiling::WindowLook& look = *m_look;
    for (Window window : m_painted)
    {
        if (IsWindow(hwnd(window)) == FALSE)
        {
            continue;
        }
        if (look.dark)
        {
            setAttribute<BOOL>(window, kDarkMode, m_systemDark ? TRUE : FALSE);
        }
        if (look.corners)
        {
            setAttribute<DWORD>(window, kCorners, 0); // DWMWCP_DEFAULT
        }
        if (look.border || look.noBorder)
        {
            setAttribute<COLORREF>(window, kBorderColor, kDefaultColor);
        }
        if (look.caption)
        {
            setAttribute<COLORREF>(window, kCaptionColor, kDefaultColor);
        }
        if (look.text)
        {
            setAttribute<COLORREF>(window, kTextColor, kDefaultColor);
        }
        if (look.backdrop)
        {
            setAttribute<DWORD>(window, kBackdrop, 0); // DWMSBT_AUTO
        }
    }
    m_painted.clear();
    m_look.reset();
}

std::optional<std::uint32_t> Looks::border() const noexcept
{
    if (!m_look)
    {
        return std::nullopt;
    }
    if (m_look->noBorder)
    {
        return 0xFFFFFFFE; // DWMWA_COLOR_NONE
    }
    if (m_look->border)
    {
        return colorref(*m_look->border);
    }
    return std::nullopt;
}

} // namespace rmw::tilerapp
