#include "rmw/video/Effects.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rmw::video
{
namespace
{

// out = a * in + t, on r, g, b.
struct Affine
{
    std::array<std::array<double, 3>, 3> a{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::array<double, 3> t{};
};

// `second` after `first`.
Affine then(const Affine& first, const Affine& second) noexcept
{
    Affine out;
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t column = 0; column < 3; ++column)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k)
            {
                sum += second.a[row][k] * first.a[k][column];
            }
            out.a[row][column] = sum;
        }
        double offset = second.t[row];
        for (std::size_t k = 0; k < 3; ++k)
        {
            offset += second.a[row][k] * first.t[k];
        }
        out.t[row] = offset;
    }
    return out;
}

Affine scaled(double k, double offset) noexcept
{
    Affine out;
    for (std::size_t i = 0; i < 3; ++i)
    {
        out.a[i] = {0, 0, 0};
        out.a[i][i] = k;
        out.t[i] = offset;
    }
    return out;
}

// Rec. 709 luminance.
constexpr std::array<double, 3> kLuminance = {0.2126, 0.7152, 0.0722};

Affine saturated(double s) noexcept
{
    Affine out;
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t column = 0; column < 3; ++column)
        {
            out.a[row][column] = ((1.0 - s) * kLuminance[column]) + (row == column ? s : 0.0);
        }
    }
    return out;
}

Affine hueTurned(double degrees) noexcept
{
    const double radians = degrees * std::numbers::pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Affine out;
    out.a = {{{0.213 + (c * 0.787) - (s * 0.213), 0.715 - (c * 0.715) - (s * 0.715), 0.072 - (c * 0.072) + (s * 0.928)},
              {0.213 - (c * 0.213) + (s * 0.143), 0.715 + (c * 0.285) + (s * 0.140), 0.072 - (c * 0.072) - (s * 0.283)},
              {0.213 - (c * 0.213) - (s * 0.787), 0.715 - (c * 0.715) + (s * 0.715), 0.072 + (c * 0.928) + (s * 0.072)}}};
    return out;
}

domain::Color resolve(const domain::ColorRef& reference, const domain::Rice& rice, domain::Color systemAccent)
{
    if (const auto* color = std::get_if<domain::Color>(&reference.value))
    {
        return *color;
    }
    const auto& name = std::get<std::string>(reference.value);
    if (const auto* entry = rice.findPalette(name))
    {
        return entry->color;
    }
    if (name == "accent")
    {
        return rice.appearance && rice.appearance->accent ? *rice.appearance->accent : systemAccent;
    }
    return systemAccent; // the parser refuses other names; never reached for a parsed rice
}

} // namespace

bool Effects::keepsColors() const noexcept
{
    return brightness == 0.0 && contrast == 1.0 && saturation == 1.0 && hue == 0.0;
}

bool Effects::none() const noexcept
{
    return keepsColors() && speed == 1.0 && blur == 0.0 && !moves() && vignette == 0.0;
}

Effects effectsOf(const domain::WallpaperEffects& effects) noexcept
{
    Effects out;
    out.brightness = effects.brightness.value_or(out.brightness);
    out.contrast = effects.contrast.value_or(out.contrast);
    out.saturation = effects.saturation.value_or(out.saturation);
    out.hue = effects.hue.value_or(out.hue);
    out.speed = effects.speed.value_or(out.speed);
    out.blur = effects.blur.value_or(out.blur);
    out.parallax = effects.parallax.value_or(out.parallax);
    out.zoom = effects.zoom.value_or(out.zoom);
    out.vignette = effects.vignette.value_or(out.vignette);
    return out;
}

ColorMatrix colorMatrix(const Effects& effects) noexcept
{
    Affine m = scaled(1.0 + effects.brightness, 0.0);
    m = then(m, scaled(effects.contrast, 0.5 * (1.0 - effects.contrast)));
    m = then(m, saturated(effects.saturation));
    m = then(m, hueTurned(effects.hue));
    ColorMatrix out{};
    for (std::size_t input = 0; input < 3; ++input)
    {
        for (std::size_t output = 0; output < 3; ++output)
        {
            out[(input * 4) + output] = static_cast<float>(m.a[output][input]);
        }
    }
    constexpr std::size_t kAlphaRow = 3;
    constexpr std::size_t kConstantRow = 4;
    out[(kAlphaRow * 4) + 3] = 1.0F; // alpha as it is
    for (std::size_t output = 0; output < 3; ++output)
    {
        out[(kConstantRow * 4) + output] = static_cast<float>(m.t[output]);
    }
    return out;
}

domain::Color applyMatrix(const ColorMatrix& matrix, domain::Color color) noexcept
{
    const std::array<double, 5> in = {color.r / 255.0, color.g / 255.0, color.b / 255.0, color.a / 255.0, 1.0};
    std::array<std::uint8_t, 4> out{};
    for (std::size_t output = 0; output < 4; ++output)
    {
        double sum = 0.0;
        for (std::size_t input = 0; input < 5; ++input)
        {
            sum += in[input] * matrix[(input * 4) + output];
        }
        out[output] = static_cast<std::uint8_t>(std::lround(std::clamp(sum, 0.0, 1.0) * 255.0));
    }
    return {out[0], out[1], out[2], out[3]};
}

Particles defaultsFor(domain::ParticleKind kind) noexcept
{
    using enum domain::ParticleKind;
    const auto make = [kind](domain::Color color, std::uint32_t count, double direction, bool glow) {
        Particles p;
        p.kind = kind;
        p.color = color;
        p.count = count;
        p.direction = direction;
        p.glow = glow;
        return p;
    };
    switch (kind)
    {
    case Sparkles:
        return make({0xFF, 0xF4, 0xD6}, 60, 90.0, true);
    case Stars:
        return make({0xFF, 0xFF, 0xFF}, 150, 0.0, false);
    case Snow:
        return make({0xFF, 0xFF, 0xFF}, 150, 90.0, false);
    case Rain:
        return make({0xA9, 0xC4, 0xFF}, 300, 100.0, false);
    case Petals:
        return make({0xFF, 0xB7, 0xD5}, 40, 110.0, false);
    case Fireflies:
        return make({0xE8, 0xFF, 0x8A}, 40, 0.0, true);
    case Magic:
        return make({0xD8, 0xB4, 0xFF}, 90, 270.0, true);
    case Notes:
        return make({0x7D, 0xF0, 0xE6}, 24, 270.0, true);
    case Hearts:
        return make({0xFF, 0x8A, 0xC2}, 24, 270.0, true);
    case Bubbles:
        return make({0xCD, 0xEF, 0xFF}, 30, 270.0, false);
    }
    return make({0xFF, 0xFF, 0xFF}, 60, 90.0, false);
}

Particles particlesOf(const domain::ParticleLayer& layer, const domain::Rice& rice, domain::Color systemAccent)
{
    Particles out = defaultsFor(layer.kind);
    if (layer.color)
    {
        out.color = resolve(*layer.color, rice, systemAccent);
    }
    out.count = layer.count.value_or(out.count);
    out.size = layer.size.value_or(out.size);
    out.speed = layer.speed.value_or(out.speed);
    out.direction = layer.direction.value_or(out.direction);
    out.glow = layer.glow.value_or(out.glow);
    return out;
}

} // namespace rmw::video
