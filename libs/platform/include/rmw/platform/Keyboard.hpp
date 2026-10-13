#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Modifier keys that stay down for Windows after the process holding their release ended badly.
// rmw-tiler's key hook holds back the release of Ctrl, Alt, Win and Shift for a moment (until its
// command is done); a tiler that crashes or is ended then leaves the key down for every app until it
// is pressed again.
namespace rmw::platform
{

struct KeyStroke
{
    std::uint16_t vk = 0; // virtual key (Windows' numbers)
    bool up = true;

    friend bool operator==(const KeyStroke&, const KeyStroke&) = default;
};

// The left and right modifiers (Shift, Ctrl, Alt, Win), as Windows numbers them.
inline constexpr std::array<std::uint16_t, 8> kModifierKeys = {0xA0, 0xA1, 0xA2, 0xA3,
                                                               0xA4, 0xA5, 0x5B, 0x5C};
inline constexpr std::uint16_t kMaskKey = 0xE8; // unassigned: Win or Alt was not pressed alone

// What to send for the modifiers `down` says are down: a key-up for each, after a mask key (down and
// up) when Win or Alt is among them, so letting go of them opens nothing (Start, a menu bar).
[[nodiscard]] std::vector<KeyStroke> modifierReleases(const std::function<bool(std::uint16_t vk)>& down);

// Sends them for the modifiers Windows counts as down now (it cannot tell those a person holds; a key
// still held is pressed again by its next repeat). Returns how many keys it let go of.
std::size_t releaseModifiers() noexcept;

} // namespace rmw::platform
