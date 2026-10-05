#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/Rice.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// config\settings.toml (docs/03 §7): the user's preferences, not part of any rice. Hotkeys and
// rules call actions from the same registry the CLI, the tray and plugins use.
namespace rmw::domain
{

inline constexpr int kSettingsSchemaVersion = 1;

// "Ctrl+Win+Alt+D" -> modifier flags + virtual-key code (Win32 values, kept as plain integers so
// the domain stays free of <windows.h>).
struct KeyChord
{
    std::uint32_t modifiers = 0; // MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8
    std::uint32_t virtualKey = 0;

    static Result<KeyChord> parse(std::string_view text);
    std::string toString() const;

    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

// What an action needs: its name and, for rice.apply, the rice folder.
struct ActionCall
{
    std::string name;     // "appearance.toggle", "rice.apply", "restore.original", ...
    std::string ricePath; // only for rice.apply (absolute folder)
};

struct Hotkey
{
    KeyChord keys;
    ActionCall action;
};

enum class RuleTrigger
{
    Time,             // every day at `at`
    OnBattery,        // the computer switches to battery
    OnAc,             // the computer is plugged in
    MonitorConnected, // a monitor matching `monitor` appears
};

struct Rule
{
    RuleTrigger trigger = RuleTrigger::Time;
    std::optional<TimeOfDay> at;            // Time
    std::optional<MonitorSelector> monitor; // MonitorConnected
    ActionCall action;
};

struct Settings
{
    bool keepInTray = false;
    std::vector<Hotkey> hotkeys;
    std::vector<Rule> rules;

    // Something needs the core to stay running.
    [[nodiscard]] bool needsResidency() const noexcept
    {
        return keepInTray || !hotkeys.empty() || !rules.empty();
    }
};

// Parses settings.toml (untrusted text like any manifest: same limits and diagnostics).
// `knownActions` lists valid action names.
[[nodiscard]] Result<Settings> parseSettings(std::string_view text, Diagnostics& diagnostics,
                                             std::span<const std::string_view> knownActions);

} // namespace rmw::domain
