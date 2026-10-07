#include "BarWindow.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <cstring>

namespace rmw::barapp
{
namespace
{

constexpr const wchar_t* kClassName = L"RicingManagerWindows.Bar";
constexpr UINT kAppBarMessage = WM_APP + 10;
constexpr UINT kRender = WM_APP + 11;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kTuckTimer = 2;
constexpr UINT_PTR kSlideTimer = 3;
constexpr UINT_PTR kStripTimer = 5;
constexpr UINT_PTR kGrowTimer = 6;
constexpr double kGrowMs = 180.0; // an auto-length bar changing length
constexpr UINT kStripCheckMs = 1000;
constexpr double kSlideMs = 200.0; // the bar sliding in, out of sight and back (M10)
constexpr UINT kTuckDelayMs = 700;
constexpr LONG kPeek = 2; // physical pixels left at the edge of an auto-hidden bar
constexpr UINT kFrameMs = 16;

// Undocumented, stable since Windows 10 and what lets a window that never takes focus keep its
// acrylic (TranslucentTB uses it on the taskbar): asks DWM to draw acrylic behind OUR window.
// Looked up at run time; when absent the tint alone is drawn.
struct AccentPolicy
{
    int state = 0;
    int flags = 0;
    DWORD gradient = 0; // AABBGGRR
    int animation = 0;
};
struct CompositionData
{
    int attribute = 0;
    void* data = nullptr;
    SIZE_T size = 0;
};
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);
constexpr int kAccentAcrylic = 4;
constexpr int kAccentPolicyAttribute = 19;

// Windows' "Animation effects" (Settings > Accessibility > Visual effects): off, nothing slides.
bool animationsWanted() noexcept
{
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) == FALSE || enabled != FALSE;
}

// Fast at first, gentle at the end.
double easeOut(double t) noexcept
{
    const double rest = 1.0 - t;
    return 1.0 - (rest * rest * rest);
}

UINT edgeOf(domain::BarPosition position) noexcept
{
    switch (position)
    {
    case domain::BarPosition::Top:
        return ABE_TOP;
    case domain::BarPosition::Left:
        return ABE_LEFT;
    case domain::BarPosition::Right:
        return ABE_RIGHT;
    case domain::BarPosition::Bottom:
        break;
    }
    return ABE_BOTTOM;
}

double scaleOf(HMONITOR monitor) noexcept
{
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        return 1.0;
    }
    return dpiX / 96.0;
}

bar::Rect fromRect(const RECT& rect) noexcept
{
    return {rect.left, rect.top, rect.right, rect.bottom};
}

} // namespace

struct BarWindow::Access
{
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lParam); // NOLINT(performance-no-int-to-ptr)
            auto* creating = static_cast<BarWindow*>(create->lpCreateParams);
            creating->m_window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating));
        }
        auto* self = reinterpret_cast<BarWindow*>( // NOLINT(performance-no-int-to-ptr)
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr)
        {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        try
        {
            return self->handle(message, wParam, lParam);
        }
        catch (const std::exception& e)
        {
            log::error("bar window message {}: {}", message, e.what());
            return 0;
        }
    }
};

BarWindow::BarWindow(domain::BarConfig config, void* monitor)
    : m_config(std::move(config))
    , m_monitor(monitor)
    , m_autohide(m_config.autohide)
{
    const auto kind = m_config.background.kind;
    m_layered =
        kind == domain::BarBackground::Kind::Solid || kind == domain::BarBackground::Kind::Transparent;
    m_translucent = kind != domain::BarBackground::Kind::Solid || m_config.radius > 0 ||
                    m_config.background.opacity.value_or(1.0) < 1.0;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Access::proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass); // fails harmlessly when it already exists

    // Off Alt+Tab and the taskbar, above other windows, and never stealing focus: clicking a
    // window button must activate that window, not the bar.
    const DWORD exStyle =
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | (m_layered ? WS_EX_LAYERED : 0UL);
    CreateWindowExW(exStyle, kClassName, L"Ricing Manager bar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                    GetModuleHandleW(nullptr), this);
    if (m_window == nullptr)
    {
        log::error("bar window: CreateWindowEx failed ({})", GetLastError());
        return;
    }
    if (!m_layered)
    {
        applyBackdrop();
    }
}

BarWindow::~BarWindow()
{
    const auto hwnd = static_cast<HWND>(m_window);
    if (m_appBar)
    {
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        SHAppBarMessage(ABM_REMOVE, &data); // gives the reserved space back
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (m_claimed && GetMonitorInfoW(static_cast<HMONITOR>(m_monitor), &info) != FALSE)
    {
        // The strip we set aside ourselves goes back too; announced, so Explorer works the area out
        // again its own way.
        RECT area = info.rcWork;
        area.left = std::min(area.left, static_cast<LONG>(m_reserved.left));
        area.top = std::min(area.top, static_cast<LONG>(m_reserved.top));
        area.right = std::max(area.right, static_cast<LONG>(m_reserved.right));
        area.bottom = std::max(area.bottom, static_cast<LONG>(m_reserved.bottom));
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &area, SPIF_SENDCHANGE);
    }
    releaseTarget();
    if (hwnd != nullptr)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
}

slint::PhysicalSize BarWindow::size()
{
    // What Slint draws: the window's pixels, supersampled.
    return slint::PhysicalSize({static_cast<std::uint32_t>(m_rect.width()) * m_factor,
                                static_cast<std::uint32_t>(m_rect.height()) * m_factor});
}

void BarWindow::set_visible(bool visible)
{
    m_visible = visible;
    if (m_window == nullptr)
    {
        return;
    }
    if (visible)
    {
        if (m_rect.empty())
        {
            // The first time: a floating bar slides in from its edge (a reserving one is an app bar:
            // it stays in its strip, where the shell put it).
            if (!m_introduced && animationsWanted() && m_config.mode == domain::BarMode::Float)
            {
                m_reveal = 0.0;
            }
            m_introduced = true;
            place();
            if (m_reveal < 1.0)
            {
                slideTo(1.0);
            }
        }
        ShowWindow(static_cast<HWND>(m_window), SW_SHOWNOACTIVATE);
        request_redraw();
    }
    else
    {
        ShowWindow(static_cast<HWND>(m_window), SW_HIDE);
    }
}

void BarWindow::request_redraw()
{
    if (!m_painting && !m_redrawPending && m_window != nullptr)
    {
        m_redrawPending = true;
        PostMessageW(static_cast<HWND>(m_window), kRender, 0, 0);
    }
}

void BarWindow::registerAppBar()
{
    if (m_appBar)
    {
        return;
    }
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(m_window);
    data.uCallbackMessage = kAppBarMessage;
    m_appBar = SHAppBarMessage(ABM_NEW, &data) != FALSE;
}

void BarWindow::place()
{
    const auto hwnd = static_cast<HWND>(m_window);
    const auto monitor = static_cast<HMONITOR>(m_monitor);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (hwnd == nullptr || GetMonitorInfoW(monitor, &info) == FALSE)
    {
        return; // the monitor is gone: the app rebuilds its bars
    }
    m_scale = scaleOf(monitor);
    m_factor = m_scale < 1.75 ? 2 : 1; // from 175% on, the pixels are small enough as they are

    // Every bar is an app bar, so it hears about fullscreen apps; only Reserve bars take space.
    registerAppBar();
    m_screen = fromRect(info.rcMonitor);
    // An auto-hidden bar takes no space and sits at the very edge, like Windows' own.
    bar::Rect area = m_autohide ? m_screen : fromRect(info.rcWork);
    if (m_config.mode == domain::BarMode::Reserve && m_appBar && !m_autohide)
    {
        const bar::Rect wanted = bar::reservation(m_config, fromRect(info.rcMonitor), m_scale);
        APPBARDATA data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uEdge = edgeOf(m_config.position);
        data.rc = {wanted.left, wanted.top, wanted.right, wanted.bottom};
        SHAppBarMessage(ABM_QUERYPOS, &data);
        // The system may move the strip inward (another bar on that edge): keep our depth.
        const LONG depth = bar::isHorizontal(m_config.position) ? wanted.height() : wanted.width();
        switch (data.uEdge)
        {
        case ABE_TOP:
            data.rc.bottom = data.rc.top + depth;
            break;
        case ABE_BOTTOM:
            data.rc.top = data.rc.bottom - depth;
            break;
        case ABE_LEFT:
            data.rc.right = data.rc.left + depth;
            break;
        default:
            data.rc.left = data.rc.right - depth;
            break;
        }
        SHAppBarMessage(ABM_SETPOS, &data);
        area = fromRect(data.rc);
        if (area != m_reserved)
        {
            log::info("bar: reserves {},{} {}x{} (asked {},{} {}x{})", area.left, area.top, area.width(),
                      area.height(), wanted.left, wanted.top, wanted.width(), wanted.height());
            m_reserved = area;
        }
        // Explorer works the work area out a moment later: looked at then (for the logs).
        SetTimer(hwnd, kStripTimer, kStripCheckMs, nullptr);
    }

    m_area = area;
    m_placedContent = m_config.length.fit && m_contentLength ? m_contentLength() : 0.0;
    m_shownContent = m_placedContent; // placed anew: no growing on the way
    KillTimer(hwnd, kGrowTimer);
    const bar::Rect rect = bar::windowRect(m_config, area, m_scale, m_placedContent);
    if (rect.empty())
    {
        return;
    }
    if (rect.width() != m_rect.width() || rect.height() != m_rect.height() || m_dc == nullptr ||
        m_surface.factor() != m_factor)
    {
        resizeTarget(rect.width(), rect.height());
    }
    m_rect = rect;
    const bar::Rect at = currentRect();
    SetWindowPos(hwnd, m_besideFullscreen ? HWND_NOTOPMOST : HWND_TOPMOST, at.left, at.top, rect.width(),
                 rect.height(), SWP_NOACTIVATE);
    if (m_autohide && !m_tucked)
    {
        SetTimer(hwnd, kTuckTimer, kTuckDelayMs * 2, nullptr); // seen once, then out of the way
    }
    window().dispatch_scale_factor_change_event(static_cast<float>(m_scale * m_factor));
    window().dispatch_resize_event(slint::LogicalSize(
        {static_cast<float>(rect.width() / m_scale), static_cast<float>(rect.height() / m_scale)}));
    request_redraw();
}

void BarWindow::checkStrip()
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (m_config.mode != domain::BarMode::Reserve || !m_appBar || m_autohide || m_reserved.empty() ||
        GetMonitorInfoW(static_cast<HMONITOR>(m_monitor), &info) == FALSE)
    {
        return;
    }
    const bar::Rect work = fromRect(info.rcWork);
    if (work.left < m_reserved.right && m_reserved.left < work.right && work.top < m_reserved.bottom &&
        m_reserved.top < work.bottom)
    {
        // Seen in the Sandbox and on the owner's PC (v1.6, at every apply): right after Windows'
        // taskbar went in or out of auto-hide, Explorer may leave a strip it granted in the work area.
        // Asking again, or the bar leaving and coming back, changed nothing; the work area set and
        // announced is put back by Explorer at once. Set without the announcement, it stays.
        log::warn("bar: Windows still counts the strip {},{} {}x{} in the work area ({},{} {}x{})", m_reserved.left,
                  m_reserved.top, m_reserved.width(), m_reserved.height(), work.left, work.top, work.width(),
                  work.height());
        claimWorkArea(work);
    }
}

void BarWindow::claimWorkArea(const bar::Rect& work)
{
    constexpr int kMaxClaimsPerMinute = 6;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_claimsSince > std::chrono::minutes(1))
    {
        m_claimsSince = now;
        m_claims = 0;
    }
    if (++m_claims > kMaxClaimsPerMinute)
    {
        if (m_claims == kMaxClaimsPerMinute + 1)
        {
            log::warn("bar: Explorer keeps taking the strip back; left to it for a minute");
        }
        return;
    }
    RECT area{work.left, work.top, work.right, work.bottom};
    switch (edgeOf(m_config.position))
    {
    case ABE_TOP:
        area.top = std::max(area.top, static_cast<LONG>(m_reserved.bottom));
        break;
    case ABE_BOTTOM:
        area.bottom = std::min(area.bottom, static_cast<LONG>(m_reserved.top));
        break;
    case ABE_LEFT:
        area.left = std::max(area.left, static_cast<LONG>(m_reserved.right));
        break;
    default:
        area.right = std::min(area.right, static_cast<LONG>(m_reserved.left));
        break;
    }
    if (area.right <= area.left || area.bottom <= area.top)
    {
        return;
    }
    if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &area, 0) == FALSE)
    {
        log::warn("bar: cannot set the work area aside ({})", GetLastError());
        return;
    }
    m_claimed = true;
    log::info("bar: the work area is now {},{} {}x{}, set by the bar", area.left, area.top, area.right - area.left,
              area.bottom - area.top);
    refitMaximized(work, fromRect(area));
}

void BarWindow::refitMaximized(const bar::Rect& before, const bar::Rect& after) const
{
    struct Context
    {
        HMONITOR monitor = nullptr;
        bar::Rect before;
        bar::Rect after;
        DWORD self = 0;
    };
    Context context{static_cast<HMONITOR>(m_monitor), before, after, GetCurrentProcessId()};
    // A maximized window keeps its borders past the work area's edges (DWM's invisible frame): each
    // edge moves by as much as the work area's did.
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands its context back as an LPARAM
            const auto& c = *reinterpret_cast<const Context*>(parameter);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            RECT rect{};
            if (pid == c.self || IsWindowVisible(window) == FALSE || IsZoomed(window) == FALSE ||
                MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != c.monitor || GetWindowRect(window, &rect) == FALSE)
            {
                return TRUE;
            }
            rect.left += c.after.left - c.before.left;
            rect.top += c.after.top - c.before.top;
            rect.right += c.after.right - c.before.right;
            rect.bottom += c.after.bottom - c.before.bottom;
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
}

bar::Rect BarWindow::tuckedRect() const noexcept
{
    bar::Rect at = m_rect;
    const auto shift = [&](std::int32_t dx, std::int32_t dy) {
        at.left += dx;
        at.right += dx;
        at.top += dy;
        at.bottom += dy;
    };
    switch (m_config.position)
    {
    case domain::BarPosition::Top:
        shift(0, m_screen.top - (m_rect.bottom - kPeek));
        break;
    case domain::BarPosition::Left:
        shift(m_screen.left - (m_rect.right - kPeek), 0);
        break;
    case domain::BarPosition::Right:
        shift(m_screen.right - kPeek - m_rect.left, 0);
        break;
    case domain::BarPosition::Bottom:
        shift(0, m_screen.bottom - kPeek - m_rect.top);
        break;
    }
    return at;
}

void BarWindow::tuck()
{
    if (!animationsWanted())
    {
        m_reveal = 0.0;
        m_revealTarget = 0.0;
        m_tucked = true;
        const bar::Rect at = currentRect();
        SetWindowPos(static_cast<HWND>(m_window), nullptr, at.left, at.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        request_redraw();
        return;
    }
    slideTo(0.0); // tucked when it gets there
}

void BarWindow::untuck()
{
    m_tucked = false;
    request_redraw(); // its transparent pixels let the mouse through again
    if (!animationsWanted())
    {
        m_reveal = 1.0;
        m_revealTarget = 1.0;
        SetWindowPos(static_cast<HWND>(m_window), nullptr, m_rect.left, m_rect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    slideTo(1.0);
}

bar::Rect BarWindow::currentRect() const noexcept
{
    if (m_reveal >= 1.0)
    {
        return m_rect;
    }
    const bar::Rect hidden = tuckedRect();
    const double t = easeOut(std::clamp(m_reveal, 0.0, 1.0));
    const auto mix = [t](std::int32_t from, std::int32_t to) {
        return from + static_cast<std::int32_t>(std::lround((to - from) * t));
    };
    return {mix(hidden.left, m_rect.left), mix(hidden.top, m_rect.top), mix(hidden.right, m_rect.right),
            mix(hidden.bottom, m_rect.bottom)};
}

void BarWindow::slideTo(double reveal)
{
    m_revealTarget = reveal;
    m_slideLast = std::chrono::steady_clock::now();
    SetTimer(static_cast<HWND>(m_window), kSlideTimer, kFrameMs, nullptr);
}

void BarWindow::slideStep()
{
    const auto now = std::chrono::steady_clock::now();
    const double step = std::chrono::duration<double, std::milli>(now - m_slideLast).count() / kSlideMs;
    m_slideLast = now;
    m_reveal = m_revealTarget > m_reveal ? std::min(m_revealTarget, m_reveal + step)
                                         : std::max(m_revealTarget, m_reveal - step);
    const bar::Rect at = currentRect();
    SetWindowPos(static_cast<HWND>(m_window), nullptr, at.left, at.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_reveal == m_revealTarget)
    {
        KillTimer(static_cast<HWND>(m_window), kSlideTimer);
        if (m_revealTarget <= 0.0)
        {
            m_tucked = true;
            request_redraw(); // tucked: nothing quite transparent, so the edge still catches the mouse
        }
    }
}

void BarWindow::growTo(double content)
{
    m_placedContent = content;
    if (!animationsWanted() || m_rect.empty())
    {
        m_shownContent = content;
        KillTimer(static_cast<HWND>(m_window), kGrowTimer);
        reshape();
        return;
    }
    // From where it is now (a change on the way starts again from there).
    m_growFrom = m_shownContent;
    m_growStart = std::chrono::steady_clock::now();
    SetTimer(static_cast<HWND>(m_window), kGrowTimer, kFrameMs, nullptr);
    growStep();
}

void BarWindow::growStep()
{
    const double t =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_growStart).count() / kGrowMs;
    if (t >= 1.0)
    {
        m_shownContent = m_placedContent;
        KillTimer(static_cast<HWND>(m_window), kGrowTimer);
    }
    else
    {
        m_shownContent = m_growFrom + ((m_placedContent - m_growFrom) * easeOut(t));
    }
    reshape();
}

void BarWindow::reshape()
{
    const bar::Rect rect = bar::windowRect(m_config, m_area, m_scale, m_shownContent);
    if (rect.empty() || rect == m_rect)
    {
        return;
    }
    if (rect.width() != m_rect.width() || rect.height() != m_rect.height())
    {
        resizeTarget(rect.width(), rect.height());
    }
    m_rect = rect;
    if (!m_layered)
    {
        // A layered window takes its new place and size with its next frame (UpdateLayeredWindow, in
        // present()): moving it now would show the old frame stretched for an instant.
        const bar::Rect at = currentRect();
        SetWindowPos(static_cast<HWND>(m_window), nullptr, at.left, at.top, rect.width(), rect.height(),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    window().dispatch_resize_event(slint::LogicalSize(
        {static_cast<float>(rect.width() / m_scale), static_cast<float>(rect.height() / m_scale)}));
    request_redraw();
}

void BarWindow::resizeTarget(std::int32_t width, std::int32_t height)
{
    // The DIB only grows: a bar changing length frame by frame draws into the one it has (present() copies
    // the rows it uses). The surface has the exact size.
    if (m_dc != nullptr && width <= m_targetWidth && height <= m_targetHeight)
    {
        m_surface.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), m_factor);
        return;
    }
    const std::int32_t dibWidth = std::max(width, m_targetWidth);
    const std::int32_t dibHeight = std::max(height, m_targetHeight);
    releaseTarget();
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = dibWidth;
    bitmap.bmiHeader.biHeight = -dibHeight; // top-down
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP section = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (dc == nullptr || section == nullptr)
    {
        log::error("bar window: cannot create a {}x{} surface", dibWidth, dibHeight);
        if (section != nullptr)
        {
            DeleteObject(section);
        }
        if (dc != nullptr)
        {
            DeleteDC(dc);
        }
        m_bits = nullptr;
        return;
    }
    m_old = SelectObject(dc, section);
    m_dc = dc;
    m_bitmap = section;
    m_targetWidth = dibWidth;
    m_targetHeight = dibHeight;
    m_surface.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), m_factor);
}

void BarWindow::releaseTarget() noexcept
{
    if (m_dc != nullptr)
    {
        SelectObject(static_cast<HDC>(m_dc), static_cast<HGDIOBJ>(m_old));
        DeleteDC(static_cast<HDC>(m_dc));
        m_dc = nullptr;
    }
    if (m_bitmap != nullptr)
    {
        DeleteObject(static_cast<HBITMAP>(m_bitmap));
        m_bitmap = nullptr;
    }
    m_bits = nullptr;
    m_targetWidth = 0;
    m_targetHeight = 0;
}

void BarWindow::render()
{
    m_redrawPending = false;
    if (!m_visible || m_bits == nullptr || m_surface.width() == 0)
    {
        return;
    }
    m_painting = true;
    m_surface.paint(
        [this](std::span<slint::Rgb8Pixel> buffer, std::size_t stride) {
            [[maybe_unused]] const auto region = m_renderer.render(buffer, stride);
        },
        m_translucent && m_matte, [this](bool white) { m_matte(white); });
    m_painting = false;
    present();
    // A bar as long as what it shows follows it (an app opens, the tray grows), smoothly.
    if (m_config.length.fit && m_contentLength && std::abs(m_contentLength() - m_placedContent) >= 1.0)
    {
        growTo(m_contentLength());
    }
    if (window().has_active_animations())
    {
        SetTimer(static_cast<HWND>(m_window), kAnimationTimer, kFrameMs,
                 nullptr); // ~60 frames/s, no busy loop
    }
}

void BarWindow::present()
{
    const auto pixels = m_surface.pixels();
    const std::size_t width = m_surface.width();
    const std::size_t height = m_surface.height();
    const auto stride = static_cast<std::size_t>(m_targetWidth);
    if (width == 0 || width > stride || std::cmp_greater(height, m_targetHeight) ||
        pixels.size() < width * height)
    {
        return;
    }
    // Row by row: the DIB may be wider than the bar is now (resizeTarget).
    const std::span bits(static_cast<std::uint32_t*>(m_bits), stride * static_cast<std::size_t>(m_targetHeight));
    for (std::size_t y = 0; y < height; ++y)
    {
        std::memcpy(&bits[y * stride], &pixels[y * width], width * sizeof(std::uint32_t));
    }
    const auto hwnd = static_cast<HWND>(m_window);
    if (m_layered)
    {
        if (m_tucked)
        {
            // Fully transparent pixels let the mouse through: the strip left at the edge must
            // still catch it, so nothing is quite transparent while tucked.
            for (std::size_t y = 0; y < height; ++y)
            {
                for (auto& pixel : bits.subspan(y * stride, width))
                {
                    pixel = (pixel >> 24) == 0 ? 0x01000000U : pixel;
                }
            }
        }
        const bar::Rect at = currentRect();
        POINT position{at.left, at.top};
        SIZE extent{m_rect.width(), m_rect.height()};
        POINT origin{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, &position, &extent, static_cast<HDC>(m_dc), &origin, 0, &blend,
                            ULW_ALPHA);
        return;
    }
    InvalidateRect(hwnd, nullptr, FALSE); // WM_PAINT copies the premultiplied pixels over the backdrop
}

void BarWindow::applyBackdrop()
{
    const auto hwnd = static_cast<HWND>(m_window);
    const DWM_WINDOW_CORNER_PREFERENCE corners = m_config.radius > 0 ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const MARGINS everywhere{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &everywhere);
    // Mica too: DWM's system backdrop shows only while the window has the focus, and a bar never
    // has it (seen on a real desktop: a flat, light surface). Acrylic under a thick theme tint
    // (drawn by Slint, see BarApp) gives the same look and keeps it.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress's contract
    const auto setComposition =
        reinterpret_cast<SetCompositionFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (setComposition == nullptr)
    {
        log::warn("bar: acrylic is not available on this Windows; the tint is drawn alone");
        return;
    }
    AccentPolicy accent;
    accent.state = kAccentAcrylic;
    accent.gradient = 0x01000000; // the tint is ours (Slint draws it): DWM adds none
    CompositionData data{kAccentPolicyAttribute, &accent, sizeof(accent)};
    setComposition(hwnd, &data);
}

slint::LogicalPosition BarWindow::logical(std::int32_t x, std::int32_t y) const
{
    return slint::LogicalPosition({static_cast<float>(x / m_scale), static_cast<float>(y / m_scale)});
}

std::intptr_t BarWindow::handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
    const auto hwnd = static_cast<HWND>(m_window);
    const auto pointer = [&](std::intptr_t value) {
        return logical(GET_X_LPARAM(value), GET_Y_LPARAM(value));
    };
    switch (message)
    {
    case kRender:
        render();
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimer)
        {
            KillTimer(hwnd, kAnimationTimer);
            request_redraw();
        }
        else if (wParam == kSlideTimer)
        {
            slideStep();
        }
        else if (wParam == kGrowTimer)
        {
            growStep();
        }
        else if (wParam == kStripTimer)
        {
            KillTimer(hwnd, kStripTimer);
            checkStrip();
        }
        else if (wParam == kTuckTimer)
        {
            KillTimer(hwnd, kTuckTimer);
            POINT cursor{};
            RECT now{};
            GetCursorPos(&cursor);
            GetWindowRect(hwnd, &now);
            if (m_autohide && !m_tucked && m_revealTarget > 0.0 && PtInRect(&now, cursor) == FALSE)
            {
                tuck();
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        if (!m_layered && m_dc != nullptr)
        {
            BitBlt(dc, 0, 0, m_rect.width(), m_rect.height(), static_cast<HDC>(m_dc), 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (m_tucked || m_revealTarget < 1.0)
        {
            untuck(); // also while it slides away
        }
        if (!m_trackingMouse)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            m_trackingMouse = TrackMouseEvent(&track) != FALSE;
        }
        window().dispatch_pointer_move_event(pointer(lParam));
        return 0;
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (m_autohide && !m_tucked)
        {
            SetTimer(hwnd, kTuckTimer, kTuckDelayMs, nullptr);
        }
        window().dispatch_pointer_exit_event();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        window().dispatch_pointer_press_event(
            pointer(lParam), message == WM_LBUTTONDOWN   ? slint::PointerEventButton::Left
                             : message == WM_RBUTTONDOWN ? slint::PointerEventButton::Right
                                                         : slint::PointerEventButton::Middle);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        ReleaseCapture();
        window().dispatch_pointer_release_event(
            pointer(lParam), message == WM_LBUTTONUP   ? slint::PointerEventButton::Left
                             : message == WM_RBUTTONUP ? slint::PointerEventButton::Right
                                                       : slint::PointerEventButton::Middle);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &at);
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        window().dispatch_pointer_scroll_event(logical(at.x, at.y), 0.0F, notches * 40.0F);
        return 0;
    }
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        place();
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA && m_config.mode == domain::BarMode::Float)
        {
            place(); // a floating bar follows the work area
        }
        else if (wParam == SPI_SETWORKAREA && m_config.mode == domain::BarMode::Reserve && !m_reserved.empty())
        {
            SetTimer(hwnd, kStripTimer, kStripCheckMs, nullptr); // Explorer worked it out again: still clear?
        }

        break;
    case kAppBarMessage:
        if (wParam == ABN_POSCHANGED)
        {
            place();
        }
        else if (wParam == ABN_FULLSCREENAPP)
        {
            // A fullscreen app (a game, a video) on our monitor: step behind it until it leaves.
            const bool opening = lParam != 0;
            const bool ours = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONULL) ==
                              static_cast<HMONITOR>(m_monitor);
            if (opening && ours)
            {
                m_besideFullscreen = true;
                SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            else if (!opening && m_besideFullscreen)
            {
                m_besideFullscreen = false;
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace rmw::barapp
