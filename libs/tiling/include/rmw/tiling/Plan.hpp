#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Color.hpp"
#include "rmw/domain/Rice.hpp"
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

// The rice's [windows] (M14), its colors resolved: what rmw-tiler paints on other apps' windows.
struct WindowLook
{
    std::optional<domain::WindowCorners> corners;
    std::optional<domain::Color> border;
    bool noBorder = false;
    std::optional<domain::Color> caption;
    std::optional<domain::Color> text;
    std::optional<domain::WindowBackdrop> backdrop;
    std::optional<bool> dark;
    std::vector<std::string> exclude; // programs, lower case

    friend bool operator==(const WindowLook&, const WindowLook&) = default;
};

// A rice's [windows] with its palette names resolved; palette.accent, when the palette has none, is
// Windows' accent now. A name the palette lacks is left out.
[[nodiscard]] WindowLook lookFromRice(const domain::WindowsLookConfig& config,
                                      std::span<const domain::PaletteEntry> palette, domain::Color accent);
[[nodiscard]] std::string toJson(const WindowLook& look);
[[nodiscard]] Result<WindowLook> parseLook(std::string_view text);

// What the core hands rmw-tiler (<data>\tiler\plan.json): the user's [tiling], the border's color
// resolved, and the rice's window look (no plan: no tiler). Tiling off with a look: the look alone.
struct Plan
{
    domain::TilingSettings settings; // its borderColor is resolved into `border` (and not kept)
    domain::Color border;            // the focused window's border: the rice's accent unless set
    std::optional<WindowLook> look;

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

// The look a tiler put on other apps' windows (the rice's [windows]), kept in the same file: a tiler
// ended before it gave them Windows' own attributes back left them painted (`rmw-tiler --restore` does it).
struct PaintedWindow
{
    WindowId window = 0;
    std::uint32_t process = 0; // a handle another process owns now is left alone

    friend bool operator==(const PaintedWindow&, const PaintedWindow&) = default;
};

struct Painted
{
    WindowLook look;
    bool systemDark = false; // apps' dark mode when the look went on: what dark title bars go back to
    std::vector<PaintedWindow> windows;

    friend bool operator==(const Painted&, const Painted&) = default;
};

// The windows to put back and, when a look is on, the painted ones.
[[nodiscard]] std::string toJson(std::span<const Original> windows, const std::optional<Painted>& painted);
// A file without a look (or of a tiler from before it kept one): nullopt.
[[nodiscard]] Result<std::optional<Painted>> parsePainted(std::string_view text);

} // namespace rmw::tiling
