#pragma once

#include "rmw/video/Effects.hpp"

#include <cstdint>
#include <vector>

// The motion of a particle layer (M12), without Windows: where every particle is, how big, how
// turned and how visible, frame after frame. rmw-wallpaper draws what this says.
namespace rmw::video
{

// One particle as it is drawn now.
struct Sprite
{
    float x = 0.0F; // its middle, in pixels
    float y = 0.0F;
    float size = 0.0F;     // pixels (rain: the streak's length)
    float rotation = 0.0F; // degrees (rain: its direction)
    float opacity = 0.0F;  // 0..1
    std::uint8_t variant = 0; // notes: which one
};

class ParticleField
{
public:
    // Sizes and speeds follow the monitor's height (they are the kind's at 1080 pixels).
    ParticleField(const Particles& layer, int width, int height, std::uint32_t seed);

    // `seconds` later (at most a tenth of a second at once: after a pause nothing jumps).
    void step(double seconds);

    [[nodiscard]] const std::vector<Sprite>& sprites() const noexcept { return m_sprites; }
    [[nodiscard]] const Particles& layer() const noexcept { return m_layer; }
    // Nothing moves or changes (speed 0): drawn once.
    [[nodiscard]] bool still() const noexcept { return m_layer.speed <= 0.0; }
    // The largest a sprite of this layer gets, in pixels.
    [[nodiscard]] float largest() const noexcept { return m_largest; }

private:
    struct Particle
    {
        float x = 0.0F; // where it is, before the sway
        float y = 0.0F;
        float speed = 0.0F;   // pixels a second along its heading
        float heading = 0.0F; // radians
        float size = 0.0F;
        float rotation = 0.0F;
        float spin = 0.0F;   // degrees a second
        float phase = 0.0F;  // 0..1
        float period = 1.0F; // seconds, of its twinkle or sway
        float age = 0.0F;
        float life = 0.0F; // seconds; 0: until it leaves the screen
        float opacity = 1.0F;
        std::uint8_t variant = 0;
    };

    [[nodiscard]] float uniform(float low, float high) noexcept;
    void spawn(Particle& particle, bool anywhere);
    [[nodiscard]] bool outside(const Particle& particle) const noexcept;
    void draw();

    Particles m_layer;
    float m_width = 0.0F;
    float m_height = 0.0F;
    float m_unit = 1.0F;   // pixels per 1080p pixel, times the layer's size
    float m_margin = 0.0F; // how far outside the screen a particle may go before it comes back
    float m_largest = 0.0F;
    float m_heading = 0.0F; // the layer's direction, radians
    std::uint64_t m_random = 0;
    std::vector<Particle> m_particles;
    std::vector<Sprite> m_sprites;
};

} // namespace rmw::video
