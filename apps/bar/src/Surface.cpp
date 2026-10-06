#include "Surface.hpp"

#include <algorithm>

namespace rmw::barapp
{

namespace
{

// 0xAARRGGBB from the pass over black and the one over white (or opaque, from black alone).
std::uint32_t premultiplied(const slint::Rgb8Pixel& black, const slint::Rgb8Pixel* white)
{
    if (white == nullptr)
    {
        return 0xFF000000U | (std::uint32_t{black.r} << 16) | (std::uint32_t{black.g} << 8) | black.b;
    }
    // Rounding makes the channels disagree by one now and then: average them.
    const int through =
        (std::max(0, white->r - black.r) + std::max(0, white->g - black.g) + std::max(0, white->b - black.b)) / 3;
    const auto alpha = static_cast<std::uint32_t>(255 - std::min(255, through));
    // Premultiplied: no channel may exceed alpha.
    const auto r = std::min<std::uint32_t>(black.r, alpha);
    const auto g = std::min<std::uint32_t>(black.g, alpha);
    const auto b = std::min<std::uint32_t>(black.b, alpha);
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

} // namespace

void Surface::resize(std::uint32_t width, std::uint32_t height, std::uint32_t factor)
{
    m_width = width;
    m_height = height;
    m_factor = std::max<std::uint32_t>(1, factor);
    // Only what a frame needs: the pass over white comes with the first translucent frame (an opaque bar
    // never draws it), and the supersampled pixels are premultiplied as they are averaged down.
    m_black.assign(drawnSize(), {});
    m_white.clear();
    m_bgra.assign(std::size_t{width} * height, 0);
}

std::size_t Surface::drawnSize() const noexcept
{
    return std::size_t{m_width} * m_factor * m_height * m_factor;
}

void Surface::paint(const Render& render, bool translucent, const Matte& matte)
{
    if (m_bgra.empty())
    {
        return;
    }
    const std::size_t stride = std::size_t{m_width} * m_factor;
    if (translucent)
    {
        matte(false);
    }
    render(m_black, stride);
    if (translucent)
    {
        if (m_white.size() != m_black.size())
        {
            m_white.assign(m_black.size(), {});
        }
        matte(true);
        render(m_white, stride);
    }
    if (m_factor == 1)
    {
        for (std::size_t i = 0; i < m_bgra.size(); ++i)
        {
            m_bgra[i] = premultiplied(m_black[i], translucent ? &m_white[i] : nullptr);
        }
        return;
    }
    // Each block of factor x factor drawn pixels, averaged (premultiplied: a plain mean is right).
    const std::uint32_t area = m_factor * m_factor;
    for (std::uint32_t y = 0; y < m_height; ++y)
    {
        for (std::uint32_t x = 0; x < m_width; ++x)
        {
            std::uint32_t a = 0;
            std::uint32_t r = 0;
            std::uint32_t g = 0;
            std::uint32_t b = 0;
            for (std::uint32_t dy = 0; dy < m_factor; ++dy)
            {
                const std::size_t row = ((std::size_t{y} * m_factor) + dy) * stride;
                for (std::uint32_t dx = 0; dx < m_factor; ++dx)
                {
                    const std::size_t at = row + (std::size_t{x} * m_factor) + dx;
                    const std::uint32_t p = premultiplied(m_black[at], translucent ? &m_white[at] : nullptr);
                    a += p >> 24;
                    r += (p >> 16) & 0xFFU;
                    g += (p >> 8) & 0xFFU;
                    b += p & 0xFFU;
                }
            }
            const auto mean = [area](std::uint32_t sum) { return (sum + (area / 2)) / area; };
            m_bgra[(std::size_t{y} * m_width) + x] = (mean(a) << 24) | (mean(r) << 16) | (mean(g) << 8) | mean(b);
        }
    }
}

} // namespace rmw::barapp
