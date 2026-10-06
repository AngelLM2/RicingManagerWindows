#pragma once

#include "Renderer.hpp"

#include "rmw/base/Error.hpp"

#include <d3d11.h>

#include <memory>
#include <string>

namespace rmw::wallpaper
{

// Draws one monitor's live wallpaper with Direct2D (M12): a picture or a color (placed, blurred and
// colored once), or a video's frames (through the effects on every frame); the slow zoom and the
// parallax on top of that; then the vignette and the particle layers. One per renderer, used on its
// thread only.
class Composer
{
public:
    Composer(std::shared_ptr<Graphics> graphics, RendererOptions options);
    ~Composer();
    Composer(const Composer&) = delete;
    Composer& operator=(const Composer&) = delete;
    Composer(Composer&&) = delete;
    Composer& operator=(Composer&&) = delete;

    // The device context, the effects and the particles' sprites; a picture is decoded and made
    // ready here. Slow (a large picture): on the renderer's thread.
    [[nodiscard]] Result<void> build();

    // A video's frames: a texture the video processor writes into (frameWidth() x frameHeight(),
    // the monitor, a little larger when the picture moves). Made once.
    [[nodiscard]] Result<ID3D11Texture2D*> frames();
    [[nodiscard]] int frameWidth() const noexcept { return m_frameWidth; }
    [[nodiscard]] int frameHeight() const noexcept { return m_frameHeight; }

    // `seconds` after the last frame: moves the particles, the slow zoom and the parallax.
    void advance(double seconds);
    // Draws into `target` (a swap chain's back buffer). Direct2D's lock must be held.
    [[nodiscard]] long draw(ID3D11Texture2D* target);

    // Frames keep changing without a video: particles that move, a zoom, the parallax.
    [[nodiscard]] bool animated() const noexcept;
    // The next frame would look like the last one (no particles or zoom, the mouse still).
    [[nodiscard]] bool settled() const noexcept;
    // "picture 3840x2160, effects, 2 particle layers" (what the status shows).
    [[nodiscard]] std::string describe() const;

    struct Parts; // the Direct2D objects (Composer.cpp)

private:
    [[nodiscard]] Result<void> buildBase();
    [[nodiscard]] Result<void> buildSprites();
    // One particle of `layer` into the bitmap being drawn: its middle at (c, c), its radius r.
    void drawShape(const video::Particles& layer, int variant, float c, float r);

    std::shared_ptr<Graphics> m_graphics;
    RendererOptions m_options;
    std::unique_ptr<Parts> m_parts;
    float m_overscan = 1.0F; // how much larger than the monitor the picture is kept
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    int m_pictureWidth = 0; // the picture as it was decoded (the status)
    int m_pictureHeight = 0;
    double m_time = 0.0;      // seconds drawn, for the slow zoom
    float m_parallaxX = 0.0F; // -1..1, where the picture leans, following the mouse
    float m_parallaxY = 0.0F;
    float m_targetX = 0.0F;
    float m_targetY = 0.0F;
    float m_drawnX = 2.0F; // where it leaned when last drawn (2: never drawn)
    float m_drawnY = 2.0F;
};

} // namespace rmw::wallpaper
