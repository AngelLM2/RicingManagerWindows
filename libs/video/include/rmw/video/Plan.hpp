#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/Rice.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What rmw-wallpaper plays (docs/04 M6): the video wallpapers of the applied rice, with files the
// core copied into its store, written by the core to <data>\wallpaper\video.json. rmw-wallpaper
// matches the monitors itself (like rmw-bar), so it follows monitors as they come and go.
namespace rmw::video
{

inline constexpr int kPlanVersion = 1;
inline constexpr std::size_t kMaxEntries = 16;

// One monitor rule of the rice, in its order. Every rule is here, videos or not: a monitor an
// earlier rule claims is not the video's ("all" takes what no other rule took).
struct Entry
{
    domain::MonitorSelector monitor;
    std::filesystem::path file; // the video, in the store; empty: not a video rule
    domain::WallpaperFit fit = domain::WallpaperFit::Fill;

    [[nodiscard]] bool isVideo() const noexcept { return !file.empty(); }
    friend bool operator==(const Entry&, const Entry&) = default;
};

struct Policy
{
    bool pauseOnFullscreen = true;
    bool pauseOnBattery = true;
    bool pauseWhenCovered = true;
    std::uint32_t fpsLimit = 30; // 0: the video's own rate
    bool mute = true;            // always muted for now: no audio path

    friend bool operator==(const Policy&, const Policy&) = default;
};

struct Plan
{
    std::vector<Entry> entries;
    Policy policy;

    [[nodiscard]] bool hasVideo() const noexcept;
    friend bool operator==(const Plan&, const Plan&) = default;
};

// Which monitors play what: for each monitor, the entry index that plays a video there, or -1.
[[nodiscard]] std::vector<int> videoFor(const Plan& plan, std::span<const domain::MonitorFacts> monitors);

// The rice's policy with the defaults of docs/03 §6 for what it leaves out.
[[nodiscard]] Policy policyOf(const domain::WallpaperPolicy& policy);

[[nodiscard]] std::string toJson(const Plan& plan);
// Read like any input (the file is in the user's folder): sizes and kinds checked.
[[nodiscard]] Result<Plan> planFromJson(std::string_view text);

} // namespace rmw::video
