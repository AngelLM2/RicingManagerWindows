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

// What an action needs: its name and, for rice.apply, the rice folder; for launch.app, the app.
struct ActionCall
{
    std::string name;     // "appearance.toggle", "rice.apply", "restore.original", ...
    std::string ricePath; // only for rice.apply (absolute folder)
    std::string app;      // only for launch.app (an id of platform::installedApps())
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

// ---- M13: the tiler (settings.toml [tiling]) ----

// How the windows of a workspace are laid out.
enum class TilingLayout : std::uint8_t
{
    Dwindle,     // each new window takes half of the last one's place, turning (bspwm, Hyprland)
    MasterStack, // the first on one side, the rest stacked on the other (dwm, xmonad)
    Columns,     // side by side, the same width
    Monocle,     // each takes the whole area (the focused one in front)
    Floating,    // nothing is placed: the windows stay where the user puts them
};

// Workspaces like Linux (ours, 1-9 on each monitor: switching hides and shows windows) or like a
// Mac (Windows' own virtual desktops, with their sliding animation).
enum class TilingMode : std::uint8_t
{
    Linux,
    Mac,
};

// What a tiler shortcut does.
enum class TilingCommand : std::uint8_t
{
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    WorkspacePrevious,
    WorkspaceNext,
    Workspace,       // + a number: go to workspace N
    SendToWorkspace, // + a number: send the focused window to workspace N
    Float,           // the window floats (or is tiled again)
    Fullscreen,      // the focused window takes the whole area (monocle), and back
    NextLayout,
    Promote,         // the focused window becomes the first (the master)
    Toggle,          // tiling off (every window back where it was) and on again
};
inline constexpr std::size_t kTilingCommandCount = 17;

// A shortcut: `keys`, or with `anyDigit` the modifiers of `keys` plus 1..9 ("Ctrl+Win+#").
struct TilingKey
{
    TilingCommand command = TilingCommand::FocusLeft;
    KeyChord keys;
    bool anyDigit = false;

    // "Ctrl+Alt+Left", "Ctrl+Win+#"; "" for none. The commands that take a number need "#".
    static Result<TilingKey> parse(TilingCommand command, std::string_view text);
    [[nodiscard]] std::string toString() const;
    // Whether both would fire on some key (a "#" covers 1..9 and the numpad's).
    [[nodiscard]] bool overlaps(const KeyChord& chord) const noexcept;
    [[nodiscard]] bool overlaps(const TilingKey& other) const noexcept;

    friend bool operator==(const TilingKey&, const TilingKey&) = default;
};

enum class TilingRuleAction : std::uint8_t
{
    Float,     // never tiled
    Ignore,    // not touched at all (no border, never hidden)
    Workspace, // opens on workspace `workspace`
};

// Per app: windows whose app (the .exe name), class and title match (every field given must).
struct TilingRule
{
    std::string app;         // "notepad.exe" (any case)
    std::string windowClass; // exactly
    std::string title;       // contained (any case)
    TilingRuleAction action = TilingRuleAction::Float;
    int workspace = 1; // Workspace: 1..9

    friend bool operator==(const TilingRule&, const TilingRule&) = default;
};

inline constexpr int kMaxWorkspaces = 9;

// The shortcuts out of the box (docs/14 §M13: with Ctrl, as the owner asked; Toggle has none).
[[nodiscard]] std::vector<TilingKey> defaultTilingKeys();

struct TilingSettings
{
    bool enabled = false;
    TilingMode mode = TilingMode::Linux;
    TilingLayout layout = TilingLayout::Dwindle;
    int workspaces = kMaxWorkspaces; // Linux: on each monitor
    int gap = 8;                     // between two windows (pixels at 100%)
    int outerGap = 8;                // between the windows and the edges of the screen
    int masterPercent = 55;          // MasterStack: the master's share of the width, 10..90
    bool border = true;              // a colored border on the focused window (Windows 11)
    std::optional<Color> borderColor; // none: the rice's accent
    bool runAsAdmin = false;          // advanced mode: admin apps' windows too (asks the UAC)
    std::vector<TilingKey> keys = defaultTilingKeys();
    std::vector<TilingRule> rules;

    friend bool operator==(const TilingSettings&, const TilingSettings&) = default;
};

[[nodiscard]] std::string_view toString(TilingLayout layout) noexcept;
[[nodiscard]] std::string_view toString(TilingMode mode) noexcept;
[[nodiscard]] std::string_view toString(TilingCommand command) noexcept; // its key in [tiling.keys]
[[nodiscard]] std::string_view toString(TilingRuleAction action) noexcept;
[[nodiscard]] std::optional<TilingLayout> tilingLayoutFromName(std::string_view name) noexcept;
[[nodiscard]] std::optional<TilingMode> tilingModeFromName(std::string_view name) noexcept;
[[nodiscard]] std::optional<TilingCommand> tilingCommandFromName(std::string_view name) noexcept;
[[nodiscard]] std::optional<TilingRuleAction> tilingRuleActionFromName(std::string_view name) noexcept;
// What makes `settings` unreadable once written, in what the tiling part holds (two commands on one
// key, a key a hotkey has while tiling is on, a rule that says no window): the settings app checks
// a change before it saves it. Nothing: fine.
struct Settings;
[[nodiscard]] std::optional<std::string> tilingProblem(const Settings& settings);
// Workspace and SendToWorkspace: the shortcut ends in "#".
[[nodiscard]] constexpr bool takesNumber(TilingCommand command) noexcept
{
    return command == TilingCommand::Workspace || command == TilingCommand::SendToWorkspace;
}

// The languages of the interface (the settings app, the tray menu). Empty: Windows' own.
inline constexpr std::string_view kLanguageEnglish = "en";
inline constexpr std::string_view kLanguagePortuguese = "pt-BR";

struct Settings
{
    bool keepInTray =
        true; // the tray icon stays (the user asked for it in the v1.0 test); off: "zero resident"
    std::string language; // [ui] language: kLanguageEnglish, kLanguagePortuguese or empty
    // [start] win_key (M11): the Win key alone opens Ricing Manager's Start, when the rice uses it.
    bool startWinKey = false;
    std::vector<Hotkey> hotkeys;
    std::vector<Rule> rules;
    TilingSettings tiling; // [tiling] (M13)

    // Something needs the core to stay running.
    [[nodiscard]] bool needsResidency() const noexcept
    {
        return keepInTray || !hotkeys.empty() || !rules.empty() || tiling.enabled;
    }
};

// Daily times on the local clock, in seconds of the day (0..86399).
inline constexpr std::int32_t kSecondsPerDay = 86'400;
// Until the clock next reads `at`:00 — 1..86400: a time that is exactly now counts as tomorrow, so
// a timer that fires on time is never armed again for the same minute.
[[nodiscard]] std::int32_t secondsUntil(TimeOfDay at, std::int32_t now) noexcept;
// Since the clock last read `at`:00 — 0..86399.
[[nodiscard]] std::int32_t secondsSince(TimeOfDay at, std::int32_t now) noexcept;

// Parses settings.toml (untrusted text like any manifest: same limits and diagnostics).
// `knownActions` lists valid action names.
[[nodiscard]] Result<Settings> parseSettings(std::string_view text, Diagnostics& diagnostics,
                                             std::span<const std::string_view> knownActions);

// settings.toml from the model (the settings app edits hotkeys and rules); it parses back to the
// same settings. Comments of an edited file are not kept.
[[nodiscard]] std::string writeSettings(const Settings& settings);

} // namespace rmw::domain
