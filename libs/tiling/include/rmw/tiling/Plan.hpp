#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Color.hpp"
#include "rmw/domain/Settings.hpp"
#include "rmw/tiling/Layout.hpp"
#include "rmw/tiling/Workspace.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::tiling
{

// What the core hands rmw-tiler (<data>\tiler\plan.json): the user's [tiling], the border's color
// resolved (no plan: no tiler).
struct Plan
{
    domain::TilingSettings settings; // its borderColor is resolved into `border` (and not kept)
    domain::Color border;            // the focused window's border: the rice's accent unless set

    friend bool operator==(const Plan&, const Plan&) = default;
};

[[nodiscard]] std::string toJson(const Plan& plan);
[[nodiscard]] Result<Plan> parsePlan(std::string_view text);

// The gaps at a monitor's scale (1.0 = 100%).
[[nodiscard]] LayoutOptions layoutOptions(const domain::TilingSettings& settings, double scale) noexcept;

// ---- shortcuts ----

struct KeyMatch
{
    domain::TilingCommand command = domain::TilingCommand::FocusLeft;
    int number = 0; // 1..9, for the shortcuts that end in "#"

    friend bool operator==(const KeyMatch&, const KeyMatch&) = default;
};

// The shortcut that `modifiers` (MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8) and the virtual key
// `vk` fire, if any.
[[nodiscard]] std::optional<KeyMatch> matchKey(std::span<const domain::TilingKey> keys, std::uint32_t modifiers,
                                               std::uint32_t vk) noexcept;

// ---- rules ----

struct WindowFacts
{
    std::string app; // the .exe's name ("notepad.exe")
    std::string windowClass;
    std::string title;
};

// The first rule whose given fields all match (app: any case; class: exactly; title: contained, any
// case).
[[nodiscard]] const domain::TilingRule* matchRule(std::span<const domain::TilingRule> rules,
                                                  const WindowFacts& facts) noexcept;

// ---- putting every window back (<data>\tiler\windows.json) ----

struct Original
{
    WindowId window = 0;
    std::uint32_t process = 0; // the window's process: a handle that was reused is left alone
    Rect rect;                 // where it was before the tiler placed it
    bool maximized = false;
    bool hidden = false; // the tiler hid it (it is on a workspace not shown)
    bool placed = true;  // the tiler moved it (a floating window it only hid keeps its place)

    friend bool operator==(const Original&, const Original&) = default;
};

[[nodiscard]] std::string toJson(std::span<const Original> windows);
[[nodiscard]] Result<std::vector<Original>> parseOriginals(std::string_view text);

} // namespace rmw::tiling
