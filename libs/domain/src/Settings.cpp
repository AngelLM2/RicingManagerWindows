#include "rmw/domain/Settings.hpp"

#include "EnumNames.hpp"
#include "TomlReader.hpp"

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
        return std::string(1, static_cast<char>(vk));
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
            settings.keepInTray = c.boolean("keep_in_tray").value_or(false);
            c.reportUnknownKeys();
        }
        readList(t, "hotkey", kMaxHotkeys, [&](TableReader& entry) { readHotkey(entry, settings); });
        readList(t, "rule", kMaxRules, [&](TableReader& entry) { readRule(entry, settings); });
        t.reportUnknownKeys();
    }

private:
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
        if (std::ranges::find(m_actions, std::string_view(*name)) == m_actions.end())
        {
            std::string known;
            for (const auto action : m_actions)
            {
                known += (known.empty() ? "" : ", ") + std::string(action);
            }
            t.error("action", std::format("unknown action '{}' (known: {})", *name, known));
            return std::nullopt;
        }
        ActionCall call{*name, {}};
        if (auto rice = t.string("rice", kMaxPathBytes))
        {
            call.ricePath = std::move(*rice);
        }
        if (call.name == "rice.apply" && call.ricePath.empty())
        {
            t.error("action", "rice.apply needs rice = \"<folder of the rice>\"");
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

} // namespace rmw::domain
