#pragma once

#include "rmw/domain/Color.hpp"
#include "rmw/domain/Rice.hpp"

#include <array>
#include <cstdint>

// What rmw-wallpaper does to a monitor's picture, video or color (M12, doc 14): the rice's
// effects and particle layers with every value it left out filled in, and the colors it names from
// its palette resolved, so the worker never needs the rice.
namespace rmw::video
{

struct Effects
{
    double brightness = 0.0; // -1..1
    double contrast = 1.0;   // 0..2
    double saturation = 1.0; // 0..2
    double hue = 0.0;        // degrees
    double speed = 1.0;      // a video's playback rate
    double blur = 0.0;       // logical pixels
    double parallax = 0.0;   // 0..1
    double zoom = 0.0;       // 0..1
    double vignette = 0.0;   // 0..1

    // No change to the colors (brightness, contrast, saturation and hue as they are).
    [[nodiscard]] bool keepsColors() const noexcept;
    // The picture moves by itself or with the mouse: frames keep coming.
    [[nodiscard]] bool moves() const noexcept { return parallax > 0.0 || zoom > 0.0; }
    // Nothing to do at all: a video plays as it is, a picture is Windows' own.
    [[nodiscard]] bool none() const noexcept;
    friend bool operator==(const Effects&, const Effects&) = default;
};

[[nodiscard]] Effects effectsOf(const domain::WallpaperEffects& effects) noexcept;

// A 5x4 color matrix as Direct2D takes it (D2D1_MATRIX_5X4_F, row major): the output color is
// [r g b a 1] times the matrix, so row i says what input i adds to each output, and row 4 is the
// constant term. Brightness scales, contrast pivots around the middle gray, saturation mixes with
// the luminance, hue turns the colors around the gray axis (the SVG feColorMatrix "hueRotate").
using ColorMatrix = std::array<float, 20>;
[[nodiscard]] ColorMatrix colorMatrix(const Effects& effects) noexcept;
// One color through the matrix (clamped): a color wallpaper is changed once, not every frame.
[[nodiscard]] domain::Color applyMatrix(const ColorMatrix& matrix, domain::Color color) noexcept;

// One layer of particles, every value known.
struct Particles
{
    domain::ParticleKind kind = domain::ParticleKind::Sparkles;
    domain::Color color;
    std::uint32_t count = 0;
    double size = 1.0;      // times the kind's own
    double speed = 1.0;     // times the kind's own
    double direction = 0.0; // degrees: 0 right, 90 down, 270 up
    bool glow = false;

    friend bool operator==(const Particles&, const Particles&) = default;
};

// What a kind looks like when the rice says nothing more.
[[nodiscard]] Particles defaultsFor(domain::ParticleKind kind) noexcept;

// The rice's layer with the kind's defaults where it says nothing. "palette.<name>" becomes the
// palette's color; "palette.accent" the rice's accent, else `systemAccent`.
[[nodiscard]] Particles particlesOf(const domain::ParticleLayer& layer, const domain::Rice& rice,
                                    domain::Color systemAccent);

} // namespace rmw::video
