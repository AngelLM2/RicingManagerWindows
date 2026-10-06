#include "MenuWindow.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <windowsx.h>

#include <array>
#include <cstring>
#include <string>

namespace rmw::barapp
{
namespace
{

constexpr const wchar_t* kClassName = L"RicingManagerWindows.Start";
constexpr UINT kRender = WM_APP + 21;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kFrameMs = 16;

// UTF-8 of one UTF-32 character.
slint::SharedString textOf(char32_t c)
{
    std::string out;
    const auto code = static_cast<std::uint32_t>(c);
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
    else
    {
        out += static_cast<char>(0xF0U | (code >> 18U));
        out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
    return {std::string_view(out)};
}

slint::SharedString textOf(std::u8string_view code)
{
    return {std::string_view(reinterpret_cast<const char*>(code.data()), code.size())};
}

// The keys Slint knows by name (the rest come as characters, from WM_CHAR).
std::optional<slint::SharedString> namedKey(std::uintptr_t virtualKey)
{
    namespace keys = slint::platform::key_codes;
    switch (virtualKey)
    {
    case VK_BACK:
        return textOf(keys::Backspace);
    case VK_TAB:
        return GetKeyState(VK_SHIFT) < 0 ? textOf(keys::Backtab) : textOf(keys::Tab);
    case VK_RETURN:
        return textOf(keys::Return);
    case VK_ESCAPE:
        return textOf(keys::Escape);
    case VK_DELETE:
        return textOf(keys::Delete);
    case VK_SHIFT:
        return textOf(keys::Shift);
    case VK_CONTROL:
        return textOf(keys::Control);
    case VK_MENU:
        return textOf(keys::Alt);
    case VK_UP:
        return textOf(keys::UpArrow);
    case VK_DOWN:
        return textOf(keys::DownArrow);
    case VK_LEFT:
        return textOf(keys::LeftArrow);
    case VK_RIGHT:
        return textOf(keys::RightArrow);
    case VK_HOME:
        return textOf(keys::Home);
    case VK_END:
        return textOf(keys::End);
    default:
        return std::nullopt;
    }
}

} // namespace

struct MenuWindow::Access
{
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lParam); // NOLINT(performance-no-int-to-ptr)
            auto* creating = static_cast<MenuWindow*>(create->lpCreateParams);
            creating->m_window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating));
        }
        auto* self = reinterpret_cast<MenuWindow*>( // NOLINT(performance-no-int-to-ptr)
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
            log::error("start menu message {}: {}", message, e.what());
            return 0;
        }
    }
};

MenuWindow::MenuWindow()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Access::proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass); // fails harmlessly when it already exists
    // Off Alt+Tab and the taskbar, above other windows; it does take the focus (the search).
    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClassName, L"Start", WS_POPUP, 0, 0, 0,
                    0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (m_window == nullptr)
    {
        log::error("start menu: CreateWindowEx failed ({})", GetLastError());
    }
    m_hiddenAt = std::chrono::steady_clock::now() - std::chrono::hours(1);
}

MenuWindow::~MenuWindow()
{
    releaseTarget();
    if (m_window != nullptr)
    {
        SetWindowLongPtrW(static_cast<HWND>(m_window), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_window));
    }
}

slint::PhysicalSize MenuWindow::size()
{
    return slint::PhysicalSize(
        {static_cast<std::uint32_t>(m_rect.width()), static_cast<std::uint32_t>(m_rect.height())});
}

void MenuWindow::set_visible(bool /*visible*/)
{
    // Shown and hidden by showAt() and hide(): Slint's show() at creation only prepares it.
}

void MenuWindow::request_redraw()
{
    if (!m_painting && !m_redrawPending && m_window != nullptr)
    {
        m_redrawPending = true;
        PostMessageW(static_cast<HWND>(m_window), kRender, 0, 0);
    }
}

void MenuWindow::showAt(const bar::Rect& rect, double scale)
{
    const auto hwnd = static_cast<HWND>(m_window);
    if (hwnd == nullptr || rect.empty())
    {
        return;
    }
    if (rect.width() != m_rect.width() || rect.height() != m_rect.height() || m_dc == nullptr)
    {
        resizeTarget(rect.width(), rect.height());
    }
    m_rect = rect;
    m_scale = scale;
    window().dispatch_scale_factor_change_event(static_cast<float>(scale));
    window().dispatch_resize_event(slint::LogicalSize(
        {static_cast<float>(rect.width() / scale), static_cast<float>(rect.height() / scale)}));
    m_shown = true;
    render(); // drawn before it shows: no frame of the last time it was open
    SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, rect.width(), rect.height(), SWP_SHOWWINDOW);
    // In front, with the keyboard: this process just had the input (a click on its bar, or the
    // keys the Win key hook sent), so Windows lets it.
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    window().dispatch_window_active_changed_event(true);
    request_redraw();
}

void MenuWindow::warmUp(const bar::Rect& rect, double scale)
{
    // A first frame drawn unseen: fonts, glyphs and layout are ready before the first open.
    if (rect.empty() || m_window == nullptr)
    {
        return;
    }
    resizeTarget(rect.width(), rect.height());
    m_rect = rect;
    m_scale = scale;
    window().dispatch_scale_factor_change_event(static_cast<float>(scale));
    window().dispatch_resize_event(slint::LogicalSize(
        {static_cast<float>(rect.width() / scale), static_cast<float>(rect.height() / scale)}));
    m_painting = true;
    m_surface.paint(
        [this](std::span<slint::Rgb8Pixel> buffer, std::size_t stride) {
            [[maybe_unused]] const auto region = m_renderer.render(buffer, stride);
        },
        true, m_matte);
    m_painting = false;
    freeTarget(); // what it was for (glyphs, layout) stays; the pixels are made again when it opens
}

void MenuWindow::freeTarget() noexcept
{
    // A menu's frames are 2 MB at 100% and 25 MB at 200%: held only while it is open.
    releaseTarget();
    m_surface.resize(0, 0);
    m_rect = {};
}

void MenuWindow::hide()
{
    if (!m_shown)
    {
        return;
    }
    m_shown = false;
    m_hiddenAt = std::chrono::steady_clock::now();
    ShowWindow(static_cast<HWND>(m_window), SW_HIDE);
    window().dispatch_window_active_changed_event(false);
    if (m_onHidden)
    {
        m_onHidden();
    }
    freeTarget();
}

std::chrono::milliseconds MenuWindow::sinceHidden() const noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_hiddenAt);
}

void MenuWindow::resizeTarget(std::int32_t width, std::int32_t height)
{
    releaseTarget();
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height; // top-down
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP section = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (dc == nullptr || section == nullptr)
    {
        log::error("start menu: cannot create a {}x{} surface", width, height);
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
    m_surface.resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
}

void MenuWindow::releaseTarget() noexcept
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
}

void MenuWindow::render()
{
    m_redrawPending = false;
    if (!m_shown || m_bits == nullptr || m_surface.width() == 0)
    {
        return;
    }
    m_painting = true;
    m_surface.paint(
        [this](std::span<slint::Rgb8Pixel> buffer, std::size_t stride) {
            [[maybe_unused]] const auto region = m_renderer.render(buffer, stride);
        },
        true, m_matte);
    m_painting = false;
    const auto pixels = m_surface.pixels();
    std::memcpy(m_bits, pixels.data(), pixels.size_bytes());
    POINT position{m_rect.left, m_rect.top};
    SIZE extent{m_rect.width(), m_rect.height()};
    POINT origin{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(static_cast<HWND>(m_window), nullptr, &position, &extent, static_cast<HDC>(m_dc), &origin,
                        0, &blend, ULW_ALPHA);
    if (window().has_active_animations())
    {
        SetTimer(static_cast<HWND>(m_window), kAnimationTimer, kFrameMs, nullptr);
    }
}

slint::LogicalPosition MenuWindow::logical(std::int32_t x, std::int32_t y) const
{
    return slint::LogicalPosition({static_cast<float>(x / m_scale), static_cast<float>(y / m_scale)});
}

void MenuWindow::key(std::uintptr_t virtualKey, bool down)
{
    std::optional<slint::SharedString> text = namedKey(virtualKey);
    // Ctrl+letter (select all, copy, paste): the letter, with Control held (WM_CHAR gives a control code).
    if (!text && GetKeyState(VK_CONTROL) < 0 && virtualKey >= 'A' && virtualKey <= 'Z')
    {
        text = textOf(static_cast<char32_t>(virtualKey - 'A' + 'a'));
    }
    if (!text)
    {
        return;
    }
    if (down)
    {
        window().dispatch_key_press_event(*text);
    }
    else
    {
        window().dispatch_key_release_event(*text);
    }
}

std::intptr_t MenuWindow::handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam)
{
    const auto hwnd = static_cast<HWND>(m_window);
    const auto pointer = [&](std::intptr_t value) { return logical(GET_X_LPARAM(value), GET_Y_LPARAM(value)); };
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
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            hide(); // a click elsewhere, Alt+Tab, another window: like Windows' own Start
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        key(wParam, true);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key(wParam, false);
        return 0;
    case WM_CHAR: {
        const auto unit = static_cast<char16_t>(wParam);
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            m_highSurrogate = unit;
            return 0;
        }
        char32_t c = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF && m_highSurrogate != 0)
        {
            c = 0x10000 + ((static_cast<char32_t>(m_highSurrogate) - 0xD800) << 10U) + (unit - 0xDC00);
        }
        m_highSurrogate = 0;
        if (c < 0x20 || c == 0x7F)
        {
            return 0; // Backspace, Enter, Esc, Ctrl+letters: came as keys
        }
        window().dispatch_key_press_event(textOf(c));
        window().dispatch_key_release_event(textOf(c));
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!m_trackingMouse)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            m_trackingMouse = TrackMouseEvent(&track) != FALSE;
        }
        window().dispatch_pointer_move_event(pointer(lParam));
        return 0;
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        window().dispatch_pointer_exit_event();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        window().dispatch_pointer_press_event(pointer(lParam), message == WM_LBUTTONDOWN
                                                                   ? slint::PointerEventButton::Left
                                                                   : slint::PointerEventButton::Right);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        ReleaseCapture();
        window().dispatch_pointer_release_event(pointer(lParam), message == WM_LBUTTONUP
                                                                     ? slint::PointerEventButton::Left
                                                                     : slint::PointerEventButton::Right);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &at);
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        window().dispatch_pointer_scroll_event(logical(at.x, at.y), 0.0F, notches * 60.0F);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace rmw::barapp
