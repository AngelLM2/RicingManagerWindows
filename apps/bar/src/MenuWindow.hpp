#pragma once

#include "Surface.hpp"

#include "rmw/bar/Geometry.hpp"

#include <slint-platform.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace rmw::barapp
{

// The window of Ricing Manager's Start menu (M11): a popup over everything that, unlike the bar,
// takes the focus (to type a search) and goes away when it loses it. Layered, per-pixel alpha:
// rounded corners and a translucent tint, drawn by Slint through the Surface.
class MenuWindow final : public slint::platform::WindowAdapter
{
public:
    MenuWindow();
    ~MenuWindow() override;
    MenuWindow(const MenuWindow&) = delete;
    MenuWindow& operator=(const MenuWindow&) = delete;
    MenuWindow(MenuWindow&&) = delete;
    MenuWindow& operator=(MenuWindow&&) = delete;

    // WindowAdapter
    slint::platform::AbstractRenderer& renderer() override { return m_renderer; }
    slint::PhysicalSize size() override;
    void set_visible(bool visible) override;
    void request_redraw() override;

    // Shows it at `rect` (physical pixels) on a monitor of that `scale`, in front, with the focus.
    void showAt(const bar::Rect& rect, double scale);
    // `lostFocus`: it went because another window took the focus (the click that did it may be on the Start
    // button, which must not open it again); Esc or an app opened from it may be followed by a click at once.
    void hide(bool lostFocus = false);
    // Draws a frame of that size unseen (the first open is then as quick as the others).
    void warmUp(const bar::Rect& rect, double scale);
    [[nodiscard]] bool shown() const noexcept { return m_shown; }
    // It went away by itself (the focus went elsewhere) this long ago: a click on the Start button
    // that made it go is not a click to open it again.
    [[nodiscard]] std::chrono::milliseconds sinceHidden() const noexcept;
    [[nodiscard]] void* hwnd() const noexcept { return m_window; }

    void setMatte(Surface::Matte matte) { m_matte = std::move(matte); }
    // Called when it hides (by itself or not): the menu resets its search, its animation.
    void setOnHidden(std::function<void()> hidden) { m_onHidden = std::move(hidden); }

private:
    struct Access;

    std::intptr_t handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);
    void render();
    void resizeTarget(std::int32_t width, std::int32_t height);
    void releaseTarget() noexcept;
    void freeTarget() noexcept; // its pixels go while it is closed

    void key(std::uintptr_t virtualKey, bool down);
    [[nodiscard]] slint::LogicalPosition logical(std::int32_t x, std::int32_t y) const;

    void* m_window = nullptr; // HWND
    slint::platform::SoftwareRenderer m_renderer{
        slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    Surface m_surface;
    Surface::Matte m_matte;
    std::function<void()> m_onHidden;
    bar::Rect m_rect;
    double m_scale = 1.0;
    bool m_shown = false;
    bool m_redrawPending = false;
    bool m_painting = false;
    bool m_trackingMouse = false;
    std::chrono::steady_clock::time_point m_hiddenAt;
    char16_t m_highSurrogate = 0; // the first half of a character typed in two WM_CHARs

    void* m_dc = nullptr;     // HDC
    void* m_bitmap = nullptr; // HBITMAP
    void* m_old = nullptr;    // HGDIOBJ
    void* m_bits = nullptr;
};

} // namespace rmw::barapp
