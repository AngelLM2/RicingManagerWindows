#include "rmw/video/Plan.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace rmw::video
{
namespace
{

using json::Json;

constexpr std::uint32_t kMaxFps = 240;
constexpr std::size_t kMaxPathBytes = 4096;

constexpr std::array kSources = {Source::None, Source::Video, Source::Image, Source::Color};
constexpr std::array kParticleKinds = {
    domain::ParticleKind::Sparkles, domain::ParticleKind::Stars,     domain::ParticleKind::Snow,
    domain::ParticleKind::Rain,     domain::ParticleKind::Petals,    domain::ParticleKind::Fireflies,
    domain::ParticleKind::Magic,    domain::ParticleKind::Notes,     domain::ParticleKind::Hearts,
    domain::ParticleKind::Bubbles};

std::optional<domain::WallpaperFit> fitFromName(std::string_view name, bool tile)
{
    for (const auto fit : {domain::WallpaperFit::Fill, domain::WallpaperFit::Fit, domain::WallpaperFit::Stretch,
                           domain::WallpaperFit::Center, domain::WallpaperFit::Tile})
    {
        if (domain::toString(fit) == name && (tile || fit != domain::WallpaperFit::Tile))
        {
            return fit;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
std::optional<E> fromName(const std::array<E, N>& values, std::string_view name)
{
    for (const E value : values)
    {
        if (toString(value) == name)
        {
            return value;
        }
    }
    return std::nullopt;
}

Result<Plan> bad(std::string_view what)
{
    return fail(ErrorCode::InvalidData, std::format("video plan: {}", what));
}

// A number in [low, high]; absent: `fallback`; anything else (text, NaN, out of range): nullopt.
std::optional<double> numberIn(const Json& object, std::string_view key, double low, double high,
                               double fallback)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return fallback;
    }
    if (!it->is_number())
    {
        return std::nullopt;
    }
    const double value = it->get<double>();
    if (std::isnan(value) || value < low || value > high)
    {
        return std::nullopt;
    }
    return value;
}

Json effectsJson(const Effects& e)
{
    return {{"brightness", e.brightness}, {"contrast", e.contrast}, {"saturation", e.saturation},
            {"hue", e.hue},               {"speed", e.speed},       {"blur", e.blur},
            {"parallax", e.parallax},     {"zoom", e.zoom},         {"vignette", e.vignette}};
}

std::optional<Effects> effectsFrom(const Json& object)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }
    const Effects e;
    const auto brightness = numberIn(object, "brightness", -1.0, 1.0, e.brightness);
    const auto contrast = numberIn(object, "contrast", 0.0, 2.0, e.contrast);
    const auto saturation = numberIn(object, "saturation", 0.0, 2.0, e.saturation);
    const auto hue = numberIn(object, "hue", -180.0, 180.0, e.hue);
    const auto speed = numberIn(object, "speed", 0.25, 4.0, e.speed);
    const auto blur = numberIn(object, "blur", 0.0, 40.0, e.blur);
    const auto parallax = numberIn(object, "parallax", 0.0, 1.0, e.parallax);
    const auto zoom = numberIn(object, "zoom", 0.0, 1.0, e.zoom);
    const auto vignette = numberIn(object, "vignette", 0.0, 1.0, e.vignette);
    if (!brightness || !contrast || !saturation || !hue || !speed || !blur || !parallax || !zoom || !vignette)
    {
        return std::nullopt;
    }
    return Effects{*brightness, *contrast, *saturation, *hue, *speed, *blur, *parallax, *zoom, *vignette};
}

Json particlesJson(const Particles& p)
{
    return {{"kind", std::string(domain::toString(p.kind))},
            {"color", p.color.toHex()},
            {"count", p.count},
            {"size", p.size},
            {"speed", p.speed},
            {"direction", p.direction},
            {"glow", p.glow}};
}

std::optional<Particles> particlesFrom(const Json& object)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }
    const auto kind = fromName(kParticleKinds, json::stringOr(object, "kind", ""));
    if (!kind)
    {
        return std::nullopt;
    }
    Particles p = defaultsFor(*kind);
    if (object.contains("color"))
    {
        auto color = domain::Color::parse(json::stringOr(object, "color", ""));
        if (!color)
        {
            return std::nullopt;
        }
        p.color = *color;
    }
    const auto count = numberIn(object, "count", 1, limits::kParticleMaxCount, p.count);
    const auto size = numberIn(object, "size", 0.25, 4.0, p.size);
    const auto speed = numberIn(object, "speed", 0.0, 4.0, p.speed);
    const auto direction = numberIn(object, "direction", -360.0, 360.0, p.direction);
    if (!count || !size || !speed || !direction)
    {
        return std::nullopt;
    }
    p.count = static_cast<std::uint32_t>(*count);
    p.size = *size;
    p.speed = *speed;
    p.direction = *direction;
    p.glow = json::boolOr(object, "glow", p.glow);
    return p;
}

} // namespace

std::string_view toString(Source source) noexcept
{
    switch (source)
    {
    case Source::None:
        return "none";
    case Source::Video:
        return "video";
    case Source::Image:
        return "image";
    case Source::Color:
        return "color";
    }
    return "none";
}

Policy policyOf(const domain::WallpaperPolicy& policy)
{
    Policy out;
    out.pauseOnFullscreen = policy.pauseOnFullscreen.value_or(true);
    out.pauseOnBattery = policy.pauseOnBattery.value_or(true);
    out.pauseWhenCovered = policy.pauseWhenCovered.value_or(true);
    out.fpsLimit = policy.fpsLimit.value_or(30);
    out.mute = policy.mute.value_or(true);
    return out;
}

bool Plan::hasLive() const noexcept
{
    return std::ranges::any_of(entries, &Entry::isLive);
}

std::vector<int> liveFor(const Plan& plan, std::span<const domain::MonitorFacts> monitors)
{
    std::vector<domain::MonitorSelector> rules;
    rules.reserve(plan.entries.size());
    for (const auto& entry : plan.entries)
    {
        rules.push_back(entry.monitor);
    }
    auto chosen = domain::resolveMonitorRules(rules, monitors);
    for (int& index : chosen)
    {
        if (index >= 0 && !plan.entries[static_cast<std::size_t>(index)].isLive())
        {
            index = -1; // Windows shows it, nothing to draw
        }
    }
    return chosen;
}

std::string toJson(const Plan& plan)
{
    Json entries = Json::array();
    for (const auto& entry : plan.entries)
    {
        Json item{{"monitor", entry.monitor.toString()},
                  {"source", std::string(toString(entry.source))},
                  {"file", utf16To8Lossy(entry.file.native())},
                  {"fit", std::string(domain::toString(entry.fit))}};
        if (entry.source == Source::Color)
        {
            item["color"] = entry.color.toHex();
        }
        if (!entry.effects.none())
        {
            item["effects"] = effectsJson(entry.effects);
        }
        if (!entry.particles.empty())
        {
            Json layers = Json::array();
            for (const auto& layer : entry.particles)
            {
                layers.push_back(particlesJson(layer));
            }
            item["particles"] = std::move(layers);
        }
        entries.push_back(std::move(item));
    }
    return json::dump(Json{{"version", kPlanVersion},
                           {"entries", std::move(entries)},
                           {"background", plan.background.toHex()},
                           {"policy",
                            {{"pause_on_fullscreen", plan.policy.pauseOnFullscreen},
                             {"pause_on_battery", plan.policy.pauseOnBattery},
                             {"pause_when_covered", plan.policy.pauseWhenCovered},
                             {"fps_limit", plan.policy.fpsLimit},
                             {"mute", plan.policy.mute}}}});
}

Result<Plan> planFromJson(std::string_view text)
{
    auto parsed = json::parse(text);
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    const auto version = root.is_object() ? json::intOr(root, "version", 0) : 0;
    if (version != 1 && version != kPlanVersion)
    {
        return bad("unknown version");
    }
    const Json entries = json::memberOr(root, "entries", Json());
    if (!entries.is_array() || entries.size() > kMaxEntries)
    {
        return bad("expected a list of entries");
    }
    Plan plan;
    for (const auto& item : entries)
    {
        if (!item.is_object())
        {
            return bad("expected an entry");
        }
        auto monitor = domain::MonitorSelector::parse(json::stringOr(item, "monitor", ""));
        const std::string file = json::stringOr(item, "file", "");
        // Version 1 had videos only: a file was a video.
        const auto source = version == 1 ? std::optional(file.empty() ? Source::None : Source::Video)
                                         : fromName(kSources, json::stringOr(item, "source", "none"));
        const auto fit = fitFromName(json::stringOr(item, "fit", "fill"), source == Source::Image);
        if (!monitor || !fit || !source || file.size() > kMaxPathBytes)
        {
            return bad("invalid entry");
        }
        Entry entry;
        entry.monitor = std::move(*monitor);
        entry.source = *source;
        entry.fit = *fit;
        if (*source == Source::Video || *source == Source::Image)
        {
            entry.file = std::filesystem::path(utf8To16Lossy(file));
            if (!entry.file.is_absolute())
            {
                return bad("a file's path must be absolute");
            }
        }
        if (*source == Source::Color)
        {
            auto color = domain::Color::parse(json::stringOr(item, "color", ""));
            if (!color)
            {
                return bad("a color entry without a color");
            }
            entry.color = *color;
        }
        if (item.contains("effects"))
        {
            auto effects = effectsFrom(item["effects"]);
            if (!effects)
            {
                return bad("invalid effects");
            }
            entry.effects = *effects;
        }
        const Json layers = json::memberOr(item, "particles", Json::array());
        if (!layers.is_array() || layers.size() > limits::kWallpaperMaxParticleLayers)
        {
            return bad("expected a short list of particle layers");
        }
        for (const auto& layer : layers)
        {
            auto particles = particlesFrom(layer);
            if (!particles)
            {
                return bad("invalid particle layer");
            }
            entry.particles.push_back(*particles);
        }
        plan.entries.push_back(std::move(entry));
    }
    if (root.contains("background"))
    {
        auto background = domain::Color::parse(json::stringOr(root, "background", ""));
        if (!background)
        {
            return bad("invalid background");
        }
        plan.background = *background;
    }
    const Json policy = json::memberOr(root, "policy", Json::object());
    plan.policy.pauseOnFullscreen = json::boolOr(policy, "pause_on_fullscreen", true);
    plan.policy.pauseOnBattery = json::boolOr(policy, "pause_on_battery", true);
    plan.policy.pauseWhenCovered = json::boolOr(policy, "pause_when_covered", true);
    plan.policy.fpsLimit = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(json::intOr(policy, "fps_limit", 30), 0, kMaxFps));
    plan.policy.mute = json::boolOr(policy, "mute", true);
    return plan;
}

} // namespace rmw::video
