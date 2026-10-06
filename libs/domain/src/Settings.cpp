#include "rmw/domain/Settings.hpp"

#include "EnumNames.hpp"
#include "TomlReader.hpp"

#include "rmw/domain/PluginApi.hpp"
#include "rmw/domain/RiceWriter.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace rmw::domain
{

using namespace detail;

namespace
{

constexpr std::size_t kMaxHotkeys = 64;
constexpr std::size_t kMaxRules = 64;
constexpr std::size_t kMaxPathBytes = 1024;
constexpr std::size_t kMaxAppIdBytes = 1024;

constexpr std::uint32_t kModAlt = 0x1;
constexpr std::uint32_t kModControl = 0x2;
constexpr std::uint32_t kModShift = 0x4;
constexpr std::uint32_t kModWin = 0x8;

struct NamedKey
{
    std::string_view name;
    std::uint32_t vk;
};

constexpr auto kNamedKeys = std::to_array<NamedKey>({
    {"Space", 0x20},       {"Enter", 0x0D},        {"Tab", 0x09},      {"Esc", 0x1B},
    {"Escape", 0x1B},      {"Backspace", 0x08},    {"Delete", 0x2E},   {"Del", 0x2E},
    {"Insert", 0x2D},      {"Ins", 0x2D},          {"Home", 0x24},     {"End", 0x23},
    {"PageUp", 0x21},      {"PgUp", 0x21},         {"PageDown", 0x22}, {"PgDn", 0x22},
    {"Left", 0x25},        {"Up", 0x26},           {"Right", 0x27},    {"Down", 0x28},
    {"Pause", 0x13},       {"PrintScreen", 0x2C},  {"Minus", 0xBD},    {"Plus", 0xBB},
    {"Equals", 0xBB},      {"Comma", 0xBC},        {"Period", 0xBE},   {"Slash", 0xBF},
    {"Backslash", 0xDC},   {"Semicolon", 0xBA},    {"Quote", 0xDE},    {"Backtick", 0xC0},
    {"BracketLeft", 0xDB}, {"BracketRight", 0xDD},
});

constexpr auto kTriggerNames = std::to_array<EnumName<RuleTrigger>>({
    {RuleTrigger::Time, "time"},
    {RuleTrigger::OnBattery, "battery"},
    {RuleTrigger::OnAc, "ac"},
    {RuleTrigger::MonitorConnected, "monitor-connected"},
});

constexpr std::size_t kMaxTilingRules = 64;
constexpr std::size_t kMaxRuleTextBytes = 260;
constexpr int kMaxGap = 64;

constexpr auto kTilingLayoutNames = std::to_array<EnumName<TilingLayout>>({
    {TilingLayout::Dwindle, "dwindle"},
    {TilingLayout::MasterStack, "master-stack"},
    {TilingLayout::Columns, "columns"},
    {TilingLayout::Monocle, "monocle"},
    {TilingLayout::Floating, "floating"},
});

constexpr auto kTilingModeNames = std::to_array<EnumName<TilingMode>>({
    {TilingMode::Linux, "linux"},
    {TilingMode::Mac, "mac"},
});

// Order matches the TilingCommand enumerators.
constexpr std::array<EnumName<TilingCommand>, kTilingCommandCount> kTilingCommandNames = {{
    {TilingCommand::FocusLeft, "focus_left"},
    {TilingCommand::FocusRight, "focus_right"},
    {TilingCommand::FocusUp, "focus_up"},
    {TilingCommand::FocusDown, "focus_down"},
    {TilingCommand::MoveLeft, "move_left"},
    {TilingCommand::MoveRight, "move_right"},
    {TilingCommand::MoveUp, "move_up"},
    {TilingCommand::MoveDown, "move_down"},
    {TilingCommand::WorkspacePrevious, "workspace_previous"},
    {TilingCommand::WorkspaceNext, "workspace_next"},
    {TilingCommand::Workspace, "workspace"},
    {TilingCommand::SendToWorkspace, "send_to_workspace"},
    {TilingCommand::Float, "float"},
    {TilingCommand::Fullscreen, "fullscreen"},
    {TilingCommand::NextLayout, "next_layout"},
    {TilingCommand::Promote, "promote"},
    {TilingCommand::Toggle, "toggle"},
}};

constexpr auto kTilingRuleActionNames = std::to_array<EnumName<TilingRuleAction>>({
    {TilingRuleAction::Float, "float"},
    {TilingRuleAction::Ignore, "ignore"},
    {TilingRuleAction::Workspace, "workspace"},
});

// Keys Windows keeps for itself, whatever a hook does.
bool reservedByWindows(const KeyChord& chord) noexcept
{
    return (chord.modifiers == kModWin && chord.virtualKey == 'L') ||
           (chord.modifiers == (kModControl | kModAlt) && chord.virtualKey == 0x2E);
}

bool isDigitKey(std::uint32_t vk) noexcept
{
    return (vk >= '1' && vk <= '9') || (vk >= 0x61 && vk <= 0x69); // 1..9 and the numpad's
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               const auto lower = [](char c) {
                   return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
               };
               return lower(x) == lower(y);
           });
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::uint32_t> keyCode(std::string_view name)
{
    if (name.size() == 1)
    {
        const char c = name[0];
        if (c >= 'a' && c <= 'z')
        {
            return static_cast<std::uint32_t>(c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            return static_cast<std::uint32_t>(c);
        }
    }
    if ((name.size() == 2 || name.size() == 3) && (name[0] == 'F' || name[0] == 'f'))
    {
        int number = 0;
        for (const char c : name.substr(1))
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            number = number * 10 + (c - '0');
        }
        if (number >= 1 && number <= 24)
        {
            return static_cast<std::uint32_t>(0x70 + number - 1);
        }
        return std::nullopt;
    }
    if (name.size() == 7 && equalsIgnoreCase(name.substr(0, 6), "Numpad") && name[6] >= '0' && name[6] <= '9')
    {
        return static_cast<std::uint32_t>(0x60 + (name[6] - '0'));
    }
    for (const auto& key : kNamedKeys)
    {
        if (equalsIgnoreCase(name, key.name))
        {
            return key.vk;
        }
    }
    return std::nullopt;
}

std::string keyName(std::uint32_t vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        return {static_cast<char>(vk)};
    }
    if (vk >= 0x70 && vk <= 0x87)
    {
        return std::format("F{}", vk - 0x70 + 1);
    }
    if (vk >= 0x60 && vk <= 0x69)
    {
        return std::format("Numpad{}", vk - 0x60);
    }
    for (const auto& key : kNamedKeys)
    {
        if (key.vk == vk)
        {
            return std::string(key.name);
        }
    }
    return std::format("0x{:02X}", vk);
}

class SettingsReader
{
public:
    SettingsReader(Diagnostics& diagnostics, std::span<const std::string_view> actions)
        : m_diagnostics(diagnostics)
        , m_actions(actions)
    {
    }

    void read(const toml::table& root, Settings& settings)
    {
        TableReader t(root, {}, m_diagnostics);
        if (const auto schema = t.integer("schema", 1, 1000); schema && *schema != kSettingsSchemaVersion)
        {
            t.error("schema", std::format("this settings file uses format {}; this version reads {}", *schema,
                                          kSettingsSchemaVersion));
        }
        if (const auto* core = t.table("core"))
        {
            TableReader c(*core, "core", m_diagnostics);
            settings.keepInTray = c.boolean("keep_in_tray").value_or(true);
            c.reportUnknownKeys();
        }
        if (const auto* ui = t.table("ui"))
        {
            TableReader u(*ui, "ui", m_diagnostics);
            if (auto language = u.string("language"))
            {
                if (*language == kLanguageEnglish || *language == kLanguagePortuguese)
                {
                    settings.language = std::move(*language);
                }
                else if (!language->empty())
                {
                    u.error("language", std::format("unknown language '{}' (known: {}, {}; leave it out for "
                                                    "Windows' own)",
                                                    *language, kLanguageEnglish, kLanguagePortuguese));
                }
            }
            u.reportUnknownKeys();
        }
        if (const auto* start = t.table("start"))
        {
            TableReader s(*start, "start", m_diagnostics);
            settings.startWinKey = s.boolean("win_key").value_or(false);
            s.reportUnknownKeys();
        }
        if (const auto* icons = t.table("icons"))
        {
            TableReader s(*icons, "icons", m_diagnostics);
            settings.machineIcons = s.boolean("machine").value_or(false);
            s.reportUnknownKeys();
        }
        readList(t, "hotkey", kMaxHotkeys, [&](TableReader& entry) { readHotkey(entry, settings); });
        readList(t, "rule", kMaxRules, [&](TableReader& entry) { readRule(entry, settings); });
        if (const auto* tiling = t.table("tiling"))
        {
            TableReader s(*tiling, "tiling", m_diagnostics);
            readTiling(s, settings);
            s.reportUnknownKeys();
        }
        t.reportUnknownKeys();
    }

private:
    void readTiling(TableReader& t, Settings& settings)
    {
        TilingSettings& tiling = settings.tiling;
        tiling.enabled = t.boolean("enabled").value_or(false);
        tiling.mode = t.enumeration<TilingMode>("mode", kTilingModeNames).value_or(TilingMode::Linux);
        tiling.layout = t.enumeration<TilingLayout>("layout", kTilingLayoutNames).value_or(TilingLayout::Dwindle);
        tiling.workspaces = static_cast<int>(t.integer("workspaces", 1, kMaxWorkspaces).value_or(kMaxWorkspaces));
        tiling.gap = static_cast<int>(t.integer("gap", 0, kMaxGap).value_or(8));
        tiling.outerGap = static_cast<int>(t.integer("outer_gap", 0, kMaxGap).value_or(8));
        tiling.masterPercent = static_cast<int>(t.integer("master_size", 10, 90).value_or(55));
        tiling.border = t.boolean("border").value_or(true);
        tiling.borderColor = t.parsed("border_color", Color::parse);
        tiling.runAsAdmin = t.boolean("run_as_admin").value_or(false);
        if (const auto* keys = t.table("keys"))
        {
            TableReader k(*keys, t.pathOf("keys"), m_diagnostics);
            readTilingKeys(k, settings);
            k.reportUnknownKeys();
        }
        readList(t, "rule", kMaxTilingRules, [&](TableReader& entry) { readTilingRule(entry, tiling); });
        if (!tiling.enabled)
        {
            return;
        }
        // The tiler's hook would take the key before the hotkey sees it.
        for (const auto& key : tiling.keys)
        {
            for (const auto& hotkey : settings.hotkeys)
            {
                if (key.overlaps(hotkey.keys))
                {
                    t.error("enabled", std::format("{} is the shortcut of {} and also a [[hotkey]] ({}); change "
                                                   "one of them ([tiling.keys] {} = \"...\")",
                                                   key.toString(), toString(key.command), hotkey.action.name,
                                                   toString(key.command)));
                }
            }
        }
    }

    // [tiling.keys]: a command left out keeps its shortcut; "" takes it away.
    void readTilingKeys(TableReader& t, Settings& settings)
    {
        std::vector<TilingKey>& keys = settings.tiling.keys;
        for (const auto& [command, name] : kTilingCommandNames)
        {
            const auto text = t.string(name);
            if (!text)
            {
                continue;
            }
            std::erase_if(keys, [&](const TilingKey& key) { return key.command == command; });
            if (text->empty())
            {
                continue;
            }
            auto key = TilingKey::parse(command, *text);
            if (!key)
            {
                t.error(name, key.error().message());
                continue;
            }
            keys.push_back(*key);
        }
        std::ranges::sort(keys, {}, &TilingKey::command);
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            const std::string_view name = toString(keys[i].command);
            for (std::size_t j = 0; j < i; ++j)
            {
                if (keys[i].overlaps(keys[j]))
                {
                    t.error(name, std::format("{} is also the shortcut of {}", keys[i].toString(),
                                              toString(keys[j].command)));
                }
            }
        }
    }

    void readTilingRule(TableReader& t, TilingSettings& tiling)
    {
        TilingRule rule;
        rule.app = t.string("app", kMaxRuleTextBytes).value_or("");
        rule.windowClass = t.string("class", kMaxRuleTextBytes).value_or("");
        rule.title = t.string("title", kMaxRuleTextBytes).value_or("");
        const auto action = t.enumeration<TilingRuleAction>("do", kTilingRuleActionNames);
        if (!action)
        {
            if (!t.raw().contains("do"))
            {
                t.missing("do");
            }
            return;
        }
        rule.action = *action;
        if (rule.app.empty() && rule.windowClass.empty() && rule.title.empty())
        {
            t.error("do", "say which windows: app, class or title");
            return;
        }
        const auto workspace = t.integer("workspace", 1, kMaxWorkspaces);
        if (rule.action == TilingRuleAction::Workspace)
        {
            if (!workspace)
            {
                if (!t.raw().contains("workspace"))
                {
                    t.missing("workspace");
                }
                return;
            }
            rule.workspace = static_cast<int>(*workspace);
        }
        tiling.rules.push_back(std::move(rule));
    }

    template <class Reader>
    void readList(TableReader& t, std::string_view key, std::size_t limit, Reader reader)
    {
        const auto* list = t.array(key, limit);
        if (list == nullptr)
        {
            return;
        }
        std::size_t index = 0;
        for (const auto& node : *list)
        {
            const std::string path = indexPath(key, index++);
            if (const auto* table = node.as_table())
            {
                TableReader entry(*table, path, m_diagnostics);
                reader(entry);
                entry.reportUnknownKeys();
            }
            else
            {
                errorAt(m_diagnostics, node, path, std::format("expected a table ([[{}]])", key));
            }
        }
    }

    std::optional<ActionCall> readAction(TableReader& t)
    {
        const auto name = t.string("action");
        if (!name)
        {
            if (!t.raw().contains("action"))
            {
                t.missing("action");
            }
            return std::nullopt;
        }
        // A plugin's action ("<plugin id>:<action>") is checked when it runs: plugins come and go.
        if (std::ranges::find(m_actions, std::string_view(*name)) == m_actions.end() &&
            !parsePluginActionName(*name))
        {
            std::string known;
            for (const auto action : m_actions)
            {
                known += (known.empty() ? "" : ", ") + std::string(action);
            }
            t.error("action", std::format("unknown action '{}' (known: {})", *name, known));
            return std::nullopt;
        }
        ActionCall call{*name, {}, {}};
        if (auto rice = t.string("rice", kMaxPathBytes))
        {
            call.ricePath = std::move(*rice);
        }
        if (call.name == "rice.apply" && call.ricePath.empty())
        {
            t.error("action", "rice.apply needs rice = \"<folder of the rice>\"");
            return std::nullopt;
        }
        if (auto app = t.string("app", kMaxAppIdBytes))
        {
            call.app = std::move(*app);
        }
        if (call.name == "launch.app" && call.app.empty())
        {
            t.error("action", "launch.app needs app = \"<id of the app>\" (rmw apps lists them)");
            return std::nullopt;
        }
        return call;
    }

    void readHotkey(TableReader& t, Settings& settings)
    {
        auto keys = t.parsed("keys", KeyChord::parse);
        if (!keys && !t.raw().contains("keys"))
        {
            t.missing("keys");
        }
        auto action = readAction(t);
        if (!keys || !action)
        {
            return;
        }
        if (std::ranges::any_of(settings.hotkeys, [&](const Hotkey& h) { return h.keys == *keys; }))
        {
            t.error("keys", std::format("{} is used by another hotkey", keys->toString()));
            return;
        }
        settings.hotkeys.push_back({*keys, std::move(*action)});
    }

    void readRule(TableReader& t, Settings& settings)
    {
        Rule rule;
        const auto trigger = t.enumeration<RuleTrigger>("when", kTriggerNames);
        if (!trigger)
        {
            if (!t.raw().contains("when"))
            {
                t.missing("when");
            }
            return;
        }
        rule.trigger = *trigger;
        if (rule.trigger == RuleTrigger::Time)
        {
            rule.at = t.parsed("at", TimeOfDay::parse);
            if (!rule.at)
            {
                if (!t.raw().contains("at"))
                {
                    t.missing("at");
                }
                return;
            }
        }
        if (rule.trigger == RuleTrigger::MonitorConnected)
        {
            rule.monitor = t.parsed("monitor", MonitorSelector::parse);
            if (!rule.monitor)
            {
                if (!t.raw().contains("monitor"))
                {
                    t.missing("monitor");
                }
                return;
            }
        }
        auto action = readAction(t);
        if (!action)
        {
            return;
        }
        rule.action = std::move(*action);
        settings.rules.push_back(std::move(rule));
    }

    Diagnostics& m_diagnostics;
    std::span<const std::string_view> m_actions;
};

} // namespace

Result<KeyChord> KeyChord::parse(std::string_view text)
{
    const auto invalid = [&](std::string_view why) {
        return fail(ErrorCode::InvalidData, std::format("invalid hotkey '{}': {}", text, why));
    };
    KeyChord chord;
    bool haveKey = false;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t plus = text.find('+', start);
        const std::string_view part =
            trim(text.substr(start, (plus == std::string_view::npos ? text.size() : plus) - start));
        if (part.empty())
        {
            return invalid("empty part");
        }
        std::uint32_t modifier = 0;
        if (equalsIgnoreCase(part, "Ctrl") || equalsIgnoreCase(part, "Control"))
        {
            modifier = kModControl;
        }
        else if (equalsIgnoreCase(part, "Alt"))
        {
            modifier = kModAlt;
        }
        else if (equalsIgnoreCase(part, "Shift"))
        {
            modifier = kModShift;
        }
        else if (equalsIgnoreCase(part, "Win") || equalsIgnoreCase(part, "Windows"))
        {
            modifier = kModWin;
        }

        if (modifier != 0)
        {
            if ((chord.modifiers & modifier) != 0)
            {
                return invalid("a modifier appears twice");
            }
            chord.modifiers |= modifier;
        }
        else
        {
            if (haveKey)
            {
                return invalid("only one key besides the modifiers");
            }
            const auto vk = keyCode(part);
            if (!vk)
            {
                return invalid(std::format("unknown key '{}'", part));
            }
            chord.virtualKey = *vk;
            haveKey = true;
        }
        if (plus == std::string_view::npos)
        {
            break;
        }
        start = plus + 1;
    }
    if (!haveKey)
    {
        return invalid("no key besides the modifiers");
    }
    // A global hotkey without a modifier would steal the key from every app (F13..F24 excepted).
    const bool extendedFunctionKey = chord.virtualKey >= 0x7C && chord.virtualKey <= 0x87;
    if (chord.modifiers == 0 && !extendedFunctionKey)
    {
        return invalid("add Ctrl, Alt, Shift or Win");
    }
    if (chord.modifiers == kModShift && !extendedFunctionKey)
    {
        return invalid("Shift alone would change typing; add Ctrl, Alt or Win");
    }
    return chord;
}

std::string KeyChord::toString() const
{
    std::string text;
    for (const auto& [flag, name] : {std::pair{kModControl, "Ctrl"}, std::pair{kModAlt, "Alt"},
                                     std::pair{kModShift, "Shift"}, std::pair{kModWin, "Win"}})
    {
        if ((modifiers & flag) != 0)
        {
            text += std::string(name) + "+";
        }
    }
    return text + keyName(virtualKey);
}

Result<TilingKey> TilingKey::parse(TilingCommand command, std::string_view text)
{
    TilingKey key;
    key.command = command;
    const std::size_t plus = text.rfind('+');
    const bool hash = plus != std::string_view::npos && trim(text.substr(plus + 1)) == "#";
    if (takesNumber(command) != hash)
    {
        return fail(ErrorCode::InvalidData,
                    hash ? std::format("'{}': only workspace and send_to_workspace take a number (#)", text)
                         : std::format("'{}': end it in +# (the workspace's number), as in Ctrl+Win+#", text));
    }
    // "Ctrl+Win+#" is checked as "Ctrl+Win+1": the same rules for the modifiers.
    auto chord = KeyChord::parse(hash ? std::string(text.substr(0, plus)) + "+1" : std::string(text));
    if (!chord)
    {
        return std::unexpected(std::move(chord.error()));
    }
    if (reservedByWindows(*chord))
    {
        return fail(ErrorCode::InvalidData, std::format("{} belongs to Windows", chord->toString()));
    }
    key.keys = *chord;
    if (hash)
    {
        key.keys.virtualKey = 0;
        key.anyDigit = true;
    }
    return key;
}

std::string TilingKey::toString() const
{
    if (!anyDigit)
    {
        return keys.toString();
    }
    KeyChord one = keys;
    one.virtualKey = '1';
    std::string text = one.toString();
    text.back() = '#';
    return text;
}

bool TilingKey::overlaps(const KeyChord& chord) const noexcept
{
    if (chord.modifiers != keys.modifiers)
    {
        return false;
    }
    return anyDigit ? isDigitKey(chord.virtualKey) : chord.virtualKey == keys.virtualKey;
}

bool TilingKey::overlaps(const TilingKey& other) const noexcept
{
    if (other.keys.modifiers != keys.modifiers)
    {
        return false;
    }
    if (anyDigit || other.anyDigit)
    {
        return anyDigit == other.anyDigit || isDigitKey(anyDigit ? other.keys.virtualKey : keys.virtualKey);
    }
    return keys.virtualKey == other.keys.virtualKey;
}

std::vector<TilingKey> defaultTilingKeys()
{
    constexpr std::uint32_t kCtrlAlt = kModControl | kModAlt;
    constexpr std::uint32_t kCtrlAltShift = kCtrlAlt | kModShift;
    constexpr std::uint32_t kCtrlWin = kModControl | kModWin;
    constexpr std::uint32_t kLeft = 0x25;
    constexpr std::uint32_t kUp = 0x26;
    constexpr std::uint32_t kRight = 0x27;
    constexpr std::uint32_t kDown = 0x28;
    using enum TilingCommand;
    return {
        {FocusLeft, {kCtrlAlt, kLeft}, false},
        {FocusRight, {kCtrlAlt, kRight}, false},
        {FocusUp, {kCtrlAlt, kUp}, false},
        {FocusDown, {kCtrlAlt, kDown}, false},
        {MoveLeft, {kCtrlAltShift, kLeft}, false},
        {MoveRight, {kCtrlAltShift, kRight}, false},
        {MoveUp, {kCtrlAltShift, kUp}, false},
        {MoveDown, {kCtrlAltShift, kDown}, false},
        {WorkspacePrevious, {kCtrlWin, kLeft}, false},
        {WorkspaceNext, {kCtrlWin, kRight}, false},
        {Workspace, {kCtrlWin, 0}, true},
        {SendToWorkspace, {kCtrlWin | kModShift, 0}, true},
        {Float, {kCtrlAlt, 'F'}, false},
        {Fullscreen, {kCtrlAlt, 'M'}, false},
        {NextLayout, {kCtrlAlt, 0x20}, false},
        {Promote, {kCtrlAlt, 0x0D}, false},
    };
}

std::optional<std::string> tilingProblem(const Settings& settings)
{
    const auto& keys = settings.tiling.keys;
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        for (std::size_t j = 0; j < i; ++j)
        {
            if (keys[i].overlaps(keys[j]))
            {
                return std::format("{} is already the key of \"{}\"", keys[i].toString(), toString(keys[j].command));
            }
        }
        const auto hotkey = std::ranges::find_if(settings.hotkeys,
                                                 [&](const Hotkey& h) { return keys[i].overlaps(h.keys); });
        if (settings.tiling.enabled && hotkey != settings.hotkeys.end())
        {
            return std::format("{} is a hotkey too ({})", keys[i].toString(), hotkey->action.name);
        }
    }
    for (const auto& rule : settings.tiling.rules)
    {
        if (rule.app.empty() && rule.windowClass.empty() && rule.title.empty())
        {
            return std::string("an app rule needs the app");
        }
    }
    return std::nullopt;
}

std::string_view toString(TilingLayout layout) noexcept
{
    return enumToName<TilingLayout>(kTilingLayoutNames, layout);
}

std::string_view toString(TilingMode mode) noexcept
{
    return enumToName<TilingMode>(kTilingModeNames, mode);
}

std::string_view toString(TilingCommand command) noexcept
{
    return enumToName<TilingCommand>(kTilingCommandNames, command);
}

std::string_view toString(TilingRuleAction action) noexcept
{
    return enumToName<TilingRuleAction>(kTilingRuleActionNames, action);
}

std::optional<TilingLayout> tilingLayoutFromName(std::string_view name) noexcept
{
    return enumFromName<TilingLayout>(kTilingLayoutNames, name);
}

std::optional<TilingMode> tilingModeFromName(std::string_view name) noexcept
{
    return enumFromName<TilingMode>(kTilingModeNames, name);
}

std::optional<TilingCommand> tilingCommandFromName(std::string_view name) noexcept
{
    return enumFromName<TilingCommand>(kTilingCommandNames, name);
}

std::optional<TilingRuleAction> tilingRuleActionFromName(std::string_view name) noexcept
{
    return enumFromName<TilingRuleAction>(kTilingRuleActionNames, name);
}

Result<Settings> parseSettings(std::string_view text, Diagnostics& diagnostics,
                               std::span<const std::string_view> knownActions)
{
    Settings settings;
    if (auto root = parseToml(text, diagnostics))
    {
        SettingsReader(diagnostics, knownActions).read(*root, settings);
    }
    if (diagnostics.hasErrors())
    {
        return fail(ErrorCode::InvalidData,
                    std::format("settings.toml is invalid: {}", diagnostics.errorSummary("settings.toml")));
    }
    return settings;
}

std::string writeSettings(const Settings& settings)
{
    const auto action = [](const ActionCall& call) {
        std::string text = "action = " + tomlQuote(call.name) + "\n";
        if (!call.ricePath.empty())
        {
            text += "rice = " + tomlQuote(call.ricePath) + "\n";
        }
        if (!call.app.empty())
        {
            text += "app = " + tomlQuote(call.app) + "\n";
        }
        return text;
    };
    std::string text = std::format("schema = {}\n\n[core]\nkeep_in_tray = {}\n", kSettingsSchemaVersion,
                                   settings.keepInTray);
    if (!settings.language.empty())
    {
        text += "\n[ui]\nlanguage = " + tomlQuote(settings.language) + "\n";
    }
    if (settings.startWinKey)
    {
        text += "\n[start]\nwin_key = true\n";
    }
    if (settings.machineIcons)
    {
        text += "\n[icons]\nmachine = true\n";
    }
    for (const auto& hotkey : settings.hotkeys)
    {
        text += "\n[[hotkey]]\nkeys = " + tomlQuote(hotkey.keys.toString()) + "\n" + action(hotkey.action);
    }
    for (const auto& rule : settings.rules)
    {
        text +=
            "\n[[rule]]\nwhen = " + tomlQuote(enumToName<RuleTrigger>(kTriggerNames, rule.trigger)) + "\n";
        if (rule.trigger == RuleTrigger::Time && rule.at)
        {
            text += std::format("at = \"{:02}:{:02}\"\n", rule.at->hour, rule.at->minute);
        }
        if (rule.trigger == RuleTrigger::MonitorConnected && rule.monitor)
        {
            text += "monitor = " + tomlQuote(rule.monitor->toString()) + "\n";
        }
        text += action(rule.action);
    }
    const TilingSettings& tiling = settings.tiling;
    if (tiling == TilingSettings{})
    {
        return text;
    }
    text += std::format("\n[tiling]\nenabled = {}\nmode = {}\nlayout = {}\nworkspaces = {}\ngap = {}\nouter_gap = "
                        "{}\nmaster_size = {}\nborder = {}\n",
                        tiling.enabled, tomlQuote(toString(tiling.mode)), tomlQuote(toString(tiling.layout)),
                        tiling.workspaces, tiling.gap, tiling.outerGap, tiling.masterPercent, tiling.border);
    if (tiling.borderColor)
    {
        text += "border_color = " + tomlQuote(tiling.borderColor->toHex()) + "\n";
    }
    if (tiling.runAsAdmin)
    {
        text += "run_as_admin = true\n";
    }
    if (tiling.keys != defaultTilingKeys())
    {
        // Every command, so the file shows what is free ("" = no shortcut).
        text += "\n[tiling.keys]\n";
        for (const auto& [command, name] : kTilingCommandNames)
        {
            const auto key = std::ranges::find(tiling.keys, command, &TilingKey::command);
            text += std::format("{} = {}\n", name, tomlQuote(key == tiling.keys.end() ? "" : key->toString()));
        }
    }
    for (const auto& rule : tiling.rules)
    {
        text += "\n[[tiling.rule]]\n";
        for (const auto& [key, value] :
             {std::pair{"app", &rule.app}, std::pair{"class", &rule.windowClass}, std::pair{"title", &rule.title}})
        {
            if (!value->empty())
            {
                text += std::format("{} = {}\n", key, tomlQuote(*value));
            }
        }
        text += "do = " + tomlQuote(toString(rule.action)) + "\n";
        if (rule.action == TilingRuleAction::Workspace)
        {
            text += std::format("workspace = {}\n", rule.workspace);
        }
    }
    return text;
}

std::int32_t secondsUntil(TimeOfDay at, std::int32_t now) noexcept
{
    const std::int32_t target = (at.hour * 3600) + (at.minute * 60);
    const std::int32_t delta = (((target - now) % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay;
    return delta == 0 ? kSecondsPerDay : delta;
}

std::int32_t secondsSince(TimeOfDay at, std::int32_t now) noexcept
{
    const std::int32_t target = (at.hour * 3600) + (at.minute * 60);
    return (((now - target) % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay;
}

} // namespace rmw::domain
