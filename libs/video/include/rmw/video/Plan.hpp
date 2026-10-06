#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/video/Effects.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What rmw-wallpaper draws (docs/04 M6, M12): the live wallpapers of the applied rice (videos, and
// pictures or colors with effects or particles), with files the core copied into its store, written
// by the core to <data>\wallpaper\video.json. rmw-wallpaper matches the monitors itself (like
// rmw-bar), so it follows monitors as they come and go.
namespace rmw::video
{

inline constexpr int kPlanVersion = 2; // 1: videos only (still read)
inline constexpr std::size_t kMaxEntries = 16;

// What a monitor shows.
enum class Source : std::uint8_t
{
    None,  // Windows' own wallpaper: nothing to draw
    Video, // `file`, played
    Image, // `file`, drawn with its effects
    Color, // `color`
};

// One monitor rule of the rice, in its order. Every rule is here, live or not: a monitor an
// earlier rule claims is not the next one's ("all" takes what no other rule took).
struct Entry
{
    domain::MonitorSelector monitor;
    Source source = Source::None;
    std::filesystem::path file; // Video, Image: in the store
    domain::Color color;        // Color: already through the color effects
    domain::WallpaperFit fit = domain::WallpaperFit::Fill;
    Effects effects;
    std::vector<Particles> particles;

    [[nodiscard]] bool isLive() const noexcept { return source != Source::None; }
    friend bool operator==(const Entry&, const Entry&) = default;
};

struct Policy
{
    bool pauseOnFullscreen = true;
    bool pauseOnBattery = true;
    bool pauseWhenCovered = true;
    std::uint32_t fpsLimit = 30; // 0: the video's own rate (and 60 for what we draw)
    bool mute = true;            // always muted for now: no audio path

    friend bool operator==(const Policy&, const Policy&) = default;
};

struct Plan
{
    std::vector<Entry> entries;
    Policy policy;
    domain::Color background; // around a picture or video that does not cover its monitor

    [[nodiscard]] bool hasLive() const noexcept;
    friend bool operator==(const Plan&, const Plan&) = default;
};

// Which monitors show what: for each monitor, the entry index drawn there, or -1 (Windows' own).
[[nodiscard]] std::vector<int> liveFor(const Plan& plan, std::span<const domain::MonitorFacts> monitors);

// The rice's policy with the defaults of docs/03 §6 for what it leaves out.
[[nodiscard]] Policy policyOf(const domain::WallpaperPolicy& policy);

[[nodiscard]] std::string toJson(const Plan& plan);
// Read like any input (the file is in the user's folder): sizes, kinds and ranges checked.
[[nodiscard]] Result<Plan> planFromJson(std::string_view text);

[[nodiscard]] std::string_view toString(Source source) noexcept;

} // namespace rmw::video
