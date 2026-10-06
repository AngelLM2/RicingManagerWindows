#include "rmw/video/Particles.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rmw::video
{
namespace
{

using domain::ParticleKind;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTau = 2.0F * kPi;
constexpr double kLongestStep = 0.1;

// What a kind does, in 1080p pixels and seconds.
struct Motion
{
    float minSize, maxSize;
    float minSpeed, maxSpeed;
    float minOpacity, maxOpacity;
    float minLife, maxLife; // 0: until it leaves the screen
    float minPeriod, maxPeriod;
    float sway;    // pixels to each side of its path
    float maxSpin; // degrees a second, either way
};

Motion motionOf(ParticleKind kind) noexcept
{
    switch (kind)
    {
    case ParticleKind::Sparkles:
        return {6, 16, 3, 8, 0.8F, 1, 1.2F, 3, 1, 1, 0, 60};
    case ParticleKind::Stars:
        return {1.5F, 3.5F, 0, 0, 0.5F, 1, 0, 0, 2, 6, 0, 0};
    case ParticleKind::Snow:
        return {2, 6, 35, 90, 0.55F, 0.95F, 0, 0, 3, 6, 18, 0};
    case ParticleKind::Rain:
        return {18, 34, 700, 1000, 0.25F, 0.55F, 0, 0, 1, 1, 0, 0};
    case ParticleKind::Petals:
        return {8, 16, 30, 70, 0.75F, 1, 0, 0, 3, 6, 30, 120};
    case ParticleKind::Fireflies:
        return {3, 6, 10, 30, 0.9F, 1, 0, 0, 2, 5, 0, 0};
    case ParticleKind::Magic:
        return {2, 5, 15, 45, 0.7F, 1, 3, 7, 0.6F, 1.6F, 8, 0};
    case ParticleKind::Notes:
        return {18, 30, 25, 45, 0.8F, 1, 5, 9, 4, 7, 20, 0};
    case ParticleKind::Hearts:
        return {12, 22, 25, 45, 0.8F, 1, 5, 9, 4, 7, 20, 0};
    case ParticleKind::Bubbles:
        return {8, 24, 30, 70, 0.5F, 0.9F, 0, 0, 2, 4, 10, 0};
    }
    return {4, 8, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
}

// Twinkles and fades: 0 at both ends of its life, 1 in the middle.
float fade(float age, float life) noexcept
{
    const float s = std::sin(kPi * std::clamp(age / life, 0.0F, 1.0F));
    return s * s;
}

} // namespace

ParticleField::ParticleField(const Particles& layer, int width, int height, std::uint32_t seed)
    : m_layer(layer)
    , m_width(static_cast<float>(std::max(width, 1)))
    , m_height(static_cast<float>(std::max(height, 1)))
    , m_unit((m_height / 1080.0F) * static_cast<float>(layer.size))
    , m_heading(static_cast<float>(layer.direction) * kPi / 180.0F)
    , m_random(0x9E3779B97F4A7C15ULL ^ seed)
{
    const Motion motion = motionOf(layer.kind);
    m_largest = motion.maxSize * m_unit;
    m_margin = m_largest + (motion.sway * m_unit) + 8.0F;
    m_particles.resize(layer.count);
    for (auto& particle : m_particles)
    {
        spawn(particle, true);
        // Not all at the start of their lives: the first frame already looks like any other.
        if (particle.life > 0.0F)
        {
            particle.age = uniform(0.0F, particle.life);
        }
    }
    draw();
}

float ParticleField::uniform(float low, float high) noexcept
{
    // splitmix64: small, fast, and the same everywhere for one seed.
    m_random += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = m_random;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    const float unit = static_cast<float>(z >> 40U) / static_cast<float>(1U << 24U); // [0, 1)
    return low + ((high - low) * unit);
}

void ParticleField::spawn(Particle& particle, bool anywhere)
{
    const Motion motion = motionOf(m_layer.kind);
    particle.size = uniform(motion.minSize, motion.maxSize) * m_unit;
    particle.speed = uniform(motion.minSpeed, motion.maxSpeed) * (m_height / 1080.0F);
    particle.heading = m_layer.kind == ParticleKind::Fireflies ? uniform(0.0F, kTau) : m_heading;
    particle.rotation = uniform(0.0F, 360.0F);
    particle.spin = motion.maxSpin > 0.0F ? uniform(-motion.maxSpin, motion.maxSpin) : 0.0F;
    particle.phase = uniform(0.0F, 1.0F);
    particle.period = uniform(motion.minPeriod, motion.maxPeriod);
    particle.age = 0.0F;
    particle.life = motion.maxLife > 0.0F ? uniform(motion.minLife, motion.maxLife) : 0.0F;
    particle.opacity = uniform(motion.minOpacity, motion.maxOpacity);
    particle.variant = static_cast<std::uint8_t>(uniform(0.0F, 2.999F));
    particle.x = uniform(0.0F, m_width);
    particle.y = uniform(0.0F, m_height);
    if (anywhere || particle.speed <= 0.0F || particle.life > 0.0F)
    {
        return;
    }
    // Back along its path, to just outside the screen: it comes in like the others.
    const float dx = -std::cos(particle.heading);
    const float dy = -std::sin(particle.heading);
    float distance = std::numeric_limits<float>::max();
    if (dx > 1e-4F)
    {
        distance = std::min(distance, (m_width + m_margin - particle.x) / dx);
    }
    else if (dx < -1e-4F)
    {
        distance = std::min(distance, (particle.x + m_margin) / -dx);
    }
    if (dy > 1e-4F)
    {
        distance = std::min(distance, (m_height + m_margin - particle.y) / dy);
    }
    else if (dy < -1e-4F)
    {
        distance = std::min(distance, (particle.y + m_margin) / -dy);
    }
    if (distance != std::numeric_limits<float>::max())
    {
        particle.x += dx * distance * 0.999F;
        particle.y += dy * distance * 0.999F;
    }
}

bool ParticleField::outside(const Particle& particle) const noexcept
{
    return particle.x < -m_margin || particle.x > m_width + m_margin || particle.y < -m_margin ||
           particle.y > m_height + m_margin;
}

void ParticleField::step(double seconds)
{
    const auto dt = static_cast<float>(std::clamp(seconds, 0.0, kLongestStep) * std::max(m_layer.speed, 0.0));
    if (dt <= 0.0F)
    {
        return;
    }
    for (auto& particle : m_particles)
    {
        particle.age += dt;
        if (m_layer.kind == ParticleKind::Fireflies)
        {
            // A wandering heading: it turns a little, this way or that.
            particle.heading += uniform(-2.5F, 2.5F) * dt;
        }
        particle.x += std::cos(particle.heading) * particle.speed * dt;
        particle.y += std::sin(particle.heading) * particle.speed * dt;
        particle.rotation = std::fmod(particle.rotation + (particle.spin * dt), 360.0F);
        if (m_layer.kind == ParticleKind::Fireflies || m_layer.kind == ParticleKind::Stars)
        {
            // They stay: what leaves one side comes back on the other.
            const float spanX = m_width + (2.0F * m_margin);
            const float spanY = m_height + (2.0F * m_margin);
            particle.x = std::fmod(particle.x + m_margin + spanX, spanX) - m_margin;
            particle.y = std::fmod(particle.y + m_margin + spanY, spanY) - m_margin;
        }
        if ((particle.life > 0.0F && particle.age >= particle.life) || outside(particle))
        {
            spawn(particle, particle.life > 0.0F);
        }
    }
    draw();
}

void ParticleField::draw()
{
    const Motion motion = motionOf(m_layer.kind);
    const float sway = motion.sway * m_unit;
    m_sprites.resize(m_particles.size());
    for (std::size_t i = 0; i < m_particles.size(); ++i)
    {
        const Particle& p = m_particles[i];
        Sprite& s = m_sprites[i];
        const float wave = std::sin(kTau * ((p.age / p.period) + p.phase));
        // The sway is across its path.
        s.x = p.x - (std::sin(p.heading) * wave * sway);
        s.y = p.y + (std::cos(p.heading) * wave * sway);
        s.size = p.size;
        s.rotation = p.rotation;
        s.variant = p.variant;
        float opacity = p.opacity;
        switch (m_layer.kind)
        {
        case ParticleKind::Sparkles:
            opacity *= fade(p.age, p.life);
            break;
        case ParticleKind::Stars:
            opacity *= 0.35F + (0.65F * (0.5F + (0.5F * wave)));
            break;
        case ParticleKind::Fireflies: {
            const float pulse = std::max(0.0F, wave);
            opacity *= 0.2F + (0.8F * pulse * pulse);
            break;
        }
        case ParticleKind::Magic:
            opacity *= fade(p.age, p.life) * (0.6F + (0.4F * std::abs(wave)));
            break;
        case ParticleKind::Notes:
        case ParticleKind::Hearts:
            opacity *= std::min(1.0F, 3.0F * fade(p.age, p.life));
            s.rotation = 15.0F * wave; // a gentle rocking
            break;
        case ParticleKind::Rain:
            s.rotation = p.heading * 180.0F / kPi;
            break;
        default:
            break;
        }
        s.opacity = std::clamp(opacity, 0.0F, 1.0F);
    }
}

} // namespace rmw::video
