#pragma once

#include "Surface.hpp"

#include "rmw/bar/Geometry.hpp"
#include "rmw/domain/Rice.hpp"

#include <slint-platform.h>

#include <cstdint>

namespace rmw::barapp
{

// One bar on one monitor: our window — app bar, position, DPI, input, presentation — with Slint
// laying out and drawing into it through its software renderer.
//
// Presentation: a layered window with per-pixel alpha (solid, transparent, rounded corners); or,
// for acrylic and mica, a normal window over a DWM backdrop, painted with premultiplied alpha.
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

    // Present target: a 32-bit top-down DIB selected into a memory DC.
    void* m_dc = nullptr;     // HDC
    void* m_bitmap = nullptr; // HBITMAP
    void* m_old = nullptr;    // HGDIOBJ
    void* m_bits = nullptr;
};

} // namespace rmw::barapp
