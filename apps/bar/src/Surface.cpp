#include "Surface.hpp"

#include <algorithm>

namespace rmw::barapp
{

void Surface::resize(std::uint32_t width, std::uint32_t height)
{
    m_width = width;
    m_height = height;
    const std::size_t count = std::size_t{width} * height;
    m_black.assign(count, {});
    m_white.assign(count, {});
    m_bgra.assign(count, 0);
}

void Surface::paint(const Render& render, bool translucent, const Matte& matte)
{
    if (m_bgra.empty())
    {
        return;
    }
    if (translucent)
    {
        matte(false);
    }
    render(m_black, m_width);
    if (!translucent)
    {
        for (std::size_t i = 0; i < m_bgra.size(); ++i)
        {
            const auto& p = m_black[i];
            m_bgra[i] = 0xFF000000U | (std::uint32_t{p.r} << 16) | (std::uint32_t{p.g} << 8) | p.b;
        }
        return;
    }
    matte(true);
    render(m_white, m_width);
    for (std::size_t i = 0; i < m_bgra.size(); ++i)
    {
        const auto& black = m_black[i];
        const auto& white = m_white[i];
        // Rounding makes the channels disagree by one now and then: average them.
        const int through = (std::max(0, white.r - black.r) + std::max(0, white.g - black.g) +
                             std::max(0, white.b - black.b)) /
                            3;
        const auto alpha = static_cast<std::uint32_t>(255 - std::min(255, through));
        // Premultiplied: no channel may exceed alpha.
        const auto r = std::min<std::uint32_t>(black.r, alpha);
        const auto g = std::min<std::uint32_t>(black.g, alpha);
        const auto b = std::min<std::uint32_t>(black.b, alpha);
        m_bgra[i] = (alpha << 24) | (r << 16) | (g << 8) | b;
    }
}

} // namespace rmw::barapp
