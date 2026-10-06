#pragma once

#include <slint-platform.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace rmw::barapp
{

// A frame of a Slint window as premultiplied BGRA — what UpdateLayeredWindow and DWM take.
// Slint's software renderer writes RGB only and clears to the window's background, so a
// translucent frame is drawn twice with that background ("matte") black, then white: a pixel lets
// the white through exactly as much as it is transparent, so alpha = 255 - (white - black), and
// the pass over black already is the premultiplied color.
//
// Supersampled (`factor` 2): Slint draws at twice the size and every 2x2 block becomes one pixel. The
// software renderer scales images by the nearest pixel and draws small text and round shapes with few
// edge tones; averaged down they come out smooth (v1.7: "texto e ícones da barra serrilhados").
class Surface
{
public:
    // Draws the whole frame into the given RGB buffer (`stride` pixels per row).
    using Render = std::function<void(std::span<slint::Rgb8Pixel> buffer, std::size_t stride)>;
    // Sets the window's background to opaque white (true) or black (false).
    using Matte = std::function<void(bool white)>;

    // `width` x `height`: the window's pixels; Slint draws `factor` times that in each direction.
    void resize(std::uint32_t width, std::uint32_t height, std::uint32_t factor = 1);
    // `translucent` = false: one pass, every pixel opaque.
    void paint(const Render& render, bool translucent, const Matte& matte);

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t factor() const noexcept { return m_factor; }
    // Row-major, top-down, 0xAARRGGBB as a little-endian BGRA byte sequence.
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return m_bgra; }

private:
    [[nodiscard]] std::size_t drawnSize() const noexcept;

    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_factor = 1;
    std::vector<slint::Rgb8Pixel> m_black;
    std::vector<slint::Rgb8Pixel> m_white; // only once a translucent frame is drawn
    std::vector<std::uint32_t> m_bgra;
};

} // namespace rmw::barapp
