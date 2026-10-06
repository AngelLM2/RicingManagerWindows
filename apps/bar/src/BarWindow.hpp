#pragma once

#include "Surface.hpp"

#include "rmw/bar/Geometry.hpp"
#include "rmw/domain/Rice.hpp"

#include <slint-platform.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace rmw::barapp
{

// One bar on one monitor: our window — app bar, position, DPI, input, presentation — with Slint
// laying out and drawing into it through its software renderer.
//
// Presentation: a layered window with per-pixel alpha (solid, transparent, rounded corners); or,
// for acrylic and mica, a normal window over DWM's acrylic, painted with premultiplied alpha.
class BarWindow final : public slint::platform::WindowAdapter
{
public:
    BarWindow(domain::BarConfig config, void* monitor);
    ~BarWindow() override;
    BarWindow(const BarWindow&) = delete;
    BarWindow& operator=(const BarWindow&) = delete;
    BarWindow(BarWindow&&) = delete;
    BarWindow& operator=(BarWindow&&) = delete;

    // WindowAdapter
    slint::platform::AbstractRenderer& renderer() override { return m_renderer; }
    slint::PhysicalSize size() override;
    void set_visible(bool visible) override;
    void request_redraw() override;

    // Where the rice wants it — again after DPI, display or app-bar changes.
    void place();
    [[nodiscard]] void* monitor() const noexcept { return m_monitor; }
    [[nodiscard]] void* hwnd() const noexcept { return m_window; }
    [[nodiscard]] const domain::BarConfig& config() const noexcept { return m_config; }
    // How to switch the window's background between black and white (Surface::Matte).
    void setMatte(Surface::Matte matte) { m_matte = std::move(matte); }
    // How long the bar wants to be, in logical pixels (length = "auto", M10).
    void setContentLength(std::function<double()> length) { m_contentLength = std::move(length); }

private:
    struct Access;

    std::intptr_t handle(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);
    void registerAppBar();
    void render();
    void present();
    void resizeTarget(std::int32_t width, std::int32_t height);
    void releaseTarget() noexcept;
    void applyBackdrop();
    // Auto-hide: tucked away but for a thin strip at the edge, back when the mouse comes.
    void tuck();
    void untuck();
    [[nodiscard]] bar::Rect tuckedRect() const noexcept;
    // Where the window is now: between tucked (0) and shown (1), as the slide goes (M10).
    [[nodiscard]] bar::Rect currentRect() const noexcept;
    void slideTo(double reveal);
    void slideStep();
    [[nodiscard]] slint::LogicalPosition logical(std::int32_t x, std::int32_t y) const;

    domain::BarConfig m_config;
    void* m_monitor;          // HMONITOR
    void* m_window = nullptr; // HWND
    slint::platform::SoftwareRenderer m_renderer{
        slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
    Surface m_surface;
    Surface::Matte m_matte;
    bar::Rect m_rect;   // where the bar is when shown
    bar::Rect m_screen; // the monitor's full rectangle (auto-hide hides past its edge)
    bar::Rect m_reserved; // the strip the shell granted (Reserve), as last logged
    double m_scale = 1.0;
    bool m_layered = true;
    bool m_translucent = true;
    bool m_appBar = false;
    bool m_visible = false;
    bool m_redrawPending = false;
    bool m_painting = false; // switching the matte asks for a redraw: not while we paint
    bool m_trackingMouse = false;
    bool m_besideFullscreen = false;
    bool m_autohide = false;
    bool m_tucked = false;
    std::function<double()> m_contentLength;
    double m_placedContent = 0.0; // the content length the window was placed for
    double m_reveal = 1.0;        // 0 tucked .. 1 shown
    double m_revealTarget = 1.0;
    std::chrono::steady_clock::time_point m_slideLast;
    bool m_introduced = false; // the first show slides in

    // Present target: a 32-bit top-down DIB selected into a memory DC.
    void* m_dc = nullptr;     // HDC
    void* m_bitmap = nullptr; // HBITMAP
    void* m_old = nullptr;    // HGDIOBJ
    void* m_bits = nullptr;
};

} // namespace rmw::barapp
