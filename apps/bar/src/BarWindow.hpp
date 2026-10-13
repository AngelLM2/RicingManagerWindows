#pragma once

#include "Surface.hpp"

#include "rmw/bar/Geometry.hpp"
#include "rmw/bar/Space.hpp"
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
    // What it holds of the screen (an app bar, a strip): `window` 0 when nothing. Whoever is told of
    // changes keeps it where the core finds it if this process ends badly.
    [[nodiscard]] bar::SpaceEntry space() const noexcept;
    void setSpaceChanged(std::function<void()> changed) { m_spaceChanged = std::move(changed); }

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
    // A Reserve bar's strip is out of the work area: logged when Explorer left it in.
    void checkStrip();
    // Windows left our strip in the work area: set it aside ourselves (`work`: the area it has now),
    // and maximized windows under the strip move out of it.
    void claimWorkArea(const bar::Rect& work);
    void refitMaximized(const bar::Rect& before, const bar::Rect& after) const;
    [[nodiscard]] bar::Rect tuckedRect() const noexcept;
    // Where the window is now: between tucked (0) and shown (1), as the slide goes (M10).
    [[nodiscard]] bar::Rect currentRect() const noexcept;
    void slideTo(double reveal);
    void slideStep();
    // A bar as long as what it shows follows it smoothly (v1.8: it grew in one jump when the tray opened or
    // an app came): from the length it has now to `content`, frame by frame.
    void growTo(double content);
    void growStep();
    // The window at the length it has now (m_shownContent) in the area place() found: no new request to the
    // shell (the strip a bar reserves does not depend on its length).
    void reshape();
    void noteSpace(); // tells m_spaceChanged when space() is not what it was
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
    bool m_claimed = false; // the bar set the work area itself (Explorer left the strip in it)
    int m_claims = 0;       // how often in the last minute: Explorer and the bar never take turns forever
    std::chrono::steady_clock::time_point m_claimsSince;
    double m_scale = 1.0;
    std::uint32_t m_factor = 1; // supersampling (Surface): 2 below 175%, where it smooths what Slint draws
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
    std::function<void()> m_spaceChanged;
    bar::SpaceEntry m_toldSpace;  // what m_spaceChanged last heard
    double m_placedContent = 0.0; // the content length the window goes to
    double m_shownContent = 0.0;  // ... and the one it has now, on the way there
    double m_growFrom = 0.0;
    std::chrono::steady_clock::time_point m_growStart;
    bar::Rect m_area; // what place() put the bar in (the work area, or the strip the shell granted)
    std::int32_t m_targetWidth = 0; // the DIB's size: a bar that grows and shrinks reuses it
    std::int32_t m_targetHeight = 0;
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
