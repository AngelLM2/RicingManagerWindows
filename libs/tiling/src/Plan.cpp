#include "rmw/tiling/Plan.hpp"

#include "rmw/json/Json.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace rmw::tiling
{
namespace
{

using json::Json;

constexpr int kPlanVersion = 1;
constexpr std::size_t kMaxPlanBytes = std::size_t{256} << 10;
constexpr std::size_t kMaxOriginalsBytes = std::size_t{4} << 20;
constexpr std::size_t kMaxKeys = 64;
constexpr std::size_t kMaxRules = 64;
constexpr std::size_t kMaxOriginals = 4096;
constexpr std::size_t kMaxRuleTextBytes = 260;
constexpr std::int64_t kMaxGap = 64;
constexpr std::int64_t kMaxCoordinate = 1 << 20;

template <class T>
Result<T> bad(std::string_view what)
{
    return fail(ErrorCode::InvalidData, std::format("tiler plan: {}", what));
}

char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

bool containsIgnoreCase(std::string_view text, std::string_view part) noexcept
{
    if (part.empty())
    {
        return true;
    }
    if (part.size() > text.size())
    {
        return false;
    }
    for (std::size_t at = 0; at + part.size() <= text.size(); ++at)
    {
        if (equalsIgnoreCase(text.substr(at, part.size()), part))
        {
            return true;
        }
    }
    return false;
}

int digitOf(std::uint32_t vk) noexcept
{
    if (vk >= '1' && vk <= '9')
    {
        return static_cast<int>(vk - '0');
    }
    if (vk >= 0x61 && vk <= 0x69) // the numpad's 1..9
    {
        return static_cast<int>(vk - 0x60);
    }
    return 0;
}

// An integer member in [low, high], or nullopt (absent, another type, out of range).
std::optional<std::int64_t> intIn(const Json& object, std::string_view key, std::int64_t low, std::int64_t high)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer())
    {
        return std::nullopt;
    }
    const auto value = it->get<std::int64_t>();
    if (value < low || value > high)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> boolIn(const Json& object, std::string_view key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean())
    {
        return std::nullopt;
    }
    return it->get<bool>();
}

std::optional<std::string> textIn(const Json& object, std::string_view key, std::size_t maxBytes)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
    {
        return std::nullopt;
    }
    auto text = it->get<std::string>();
    if (text.size() > maxBytes)
    {
        return std::nullopt;
    }
    return text;
}

Result<domain::TilingKey> keyFrom(const Json& entry)
{
    if (!entry.is_object())
    {
        return bad<domain::TilingKey>("a key is not an object");
    }
    const auto name = textIn(entry, "command", 64);
    const auto command = name ? domain::tilingCommandFromName(*name) : std::nullopt;
    const auto modifiers = intIn(entry, "modifiers", 0, 0xF);
    const auto key = intIn(entry, "key", 0, 0xFE);
    const auto anyDigit = boolIn(entry, "anyDigit");
    if (!command || !modifiers || !key || !anyDigit || domain::takesNumber(*command) != *anyDigit ||
        (*modifiers == 0) || (*key == 0) != *anyDigit)
    {
        return bad<domain::TilingKey>("a key is malformed");
    }
    domain::TilingKey out;
    out.command = *command;
    out.keys.modifiers = static_cast<std::uint32_t>(*modifiers);
    out.keys.virtualKey = static_cast<std::uint32_t>(*key);
    out.anyDigit = *anyDigit;
    return out;
}

Result<domain::TilingRule> ruleFrom(const Json& entry)
{
    if (!entry.is_object())
    {
        return bad<domain::TilingRule>("a rule is not an object");
    }
    const auto app = textIn(entry, "app", kMaxRuleTextBytes);
    const auto windowClass = textIn(entry, "class", kMaxRuleTextBytes);
    const auto title = textIn(entry, "title", kMaxRuleTextBytes);
    const auto name = textIn(entry, "do", 32);
    const auto action = name ? domain::tilingRuleActionFromName(*name) : std::nullopt;
    const auto workspace = intIn(entry, "workspace", 1, domain::kMaxWorkspaces);
    if (!app || !windowClass || !title || !action || !workspace ||
        (app->empty() && windowClass->empty() && title->empty()))
    {
        return bad<domain::TilingRule>("a rule is malformed");
    }
    return domain::TilingRule{*app, *windowClass, *title, *action, static_cast<int>(*workspace)};
}

Json lookJson(const WindowLook& look)
{
    Json out = Json::object();
    if (look.corners)
    {
        out["corners"] = std::string(domain::toString(*look.corners));
    }
    if (look.noBorder)
    {
        out["border"] = "none";
    }
    else if (look.border)
    {
        out["border"] = look.border->toHex();
    }
    if (look.caption)
    {
        out["caption"] = look.caption->toHex();
    }
    if (look.text)
    {
        out["text"] = look.text->toHex();
    }
    if (look.backdrop)
    {
        out["backdrop"] = std::string(domain::toString(*look.backdrop));
    }
    if (look.dark)
    {
        out["dark"] = *look.dark;
    }
    out["exclude"] = look.exclude;
    return out;
}

Result<WindowLook> lookFrom(const Json& object)
{
    if (!object.is_object())
    {
        return bad<WindowLook>("the look is not an object");
    }
    WindowLook look;
    const auto color = [&](std::string_view key, std::optional<domain::Color>& into) -> bool {
        if (!object.contains(key))
        {
            return true;
        }
        const auto text = textIn(object, key, 16);
        auto parsed = text ? domain::Color::parse(*text) : Result<domain::Color>(fail(ErrorCode::InvalidData, ""));
        if (!parsed)
        {
            return false;
        }
        into = *parsed;
        return true;
    };
    if (object.contains("corners"))
    {
        const auto name = textIn(object, "corners", 16);
        look.corners = name ? domain::windowCornersFromName(*name) : std::nullopt;
        if (!look.corners)
        {
            return bad<WindowLook>("the look's corners");
        }
    }
    if (object.contains("backdrop"))
    {
        const auto name = textIn(object, "backdrop", 16);
        look.backdrop = name ? domain::windowBackdropFromName(*name) : std::nullopt;
        if (!look.backdrop)
        {
            return bad<WindowLook>("the look's backdrop");
        }
    }
    look.noBorder = textIn(object, "border", 16) == "none";
    if ((!look.noBorder && !color("border", look.border)) || !color("caption", look.caption) ||
        !color("text", look.text))
    {
        return bad<WindowLook>("a color of the look");
    }
    look.dark = boolIn(object, "dark");
    const auto exclude = object.find("exclude");
    if (exclude != object.end())
    {
        if (!exclude->is_array() || exclude->size() > kMaxRules)
        {
            return bad<WindowLook>("the look's exclude must be a short list");
        }
        for (const auto& name : *exclude)
        {
            if (!name.is_string() || !domain::isValidProgramName(name.get<std::string>()))
            {
                return bad<WindowLook>("the look's exclude names a program badly");
            }
            look.exclude.push_back(name.get<std::string>());
        }
    }
    return look;
}

} // namespace

WindowLook lookFromRice(const domain::WindowsLookConfig& config, std::span<const domain::PaletteEntry> palette,
                        domain::Color accent)
{
    const auto resolve = [&](const std::optional<domain::ColorRef>& color) -> std::optional<domain::Color> {
        if (!color)
        {
            return std::nullopt;
        }
        if (const auto* direct = std::get_if<domain::Color>(&color->value))
        {
            return *direct;
        }
        const auto& name = std::get<std::string>(color->value);
        const auto entry = std::ranges::find(palette, name, &domain::PaletteEntry::name);
        if (entry == palette.end())
        {
            return name == "accent" ? std::optional(accent) : std::nullopt;
        }
        return entry->color;
    };
    WindowLook look;
    look.corners = config.corners;
    look.border = resolve(config.border);
    look.noBorder = config.noBorder;
    look.caption = resolve(config.caption);
    look.text = resolve(config.title);
    look.backdrop = config.backdrop;
    look.dark = config.dark;
    look.exclude = config.exclude;
    return look;
}

std::string toJson(const WindowLook& look)
{
    return json::dumpPretty(lookJson(look));
}

Result<WindowLook> parseLook(std::string_view text)
{
    auto parsed = json::parse(text, json::ParseLimits{.maxBytes = kMaxPlanBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    return lookFrom(*parsed);
}

std::string toJson(const Plan& plan)
{
    const domain::TilingSettings& s = plan.settings;
    Json keys = Json::array();
    for (const auto& key : s.keys)
    {
        keys.push_back({{"command", std::string(domain::toString(key.command))},
                        {"modifiers", key.keys.modifiers},
                        {"key", key.keys.virtualKey},
                        {"anyDigit", key.anyDigit}});
    }
    Json rules = Json::array();
    for (const auto& rule : s.rules)
    {
        rules.push_back({{"app", rule.app},
                         {"class", rule.windowClass},
                         {"title", rule.title},
                         {"do", std::string(domain::toString(rule.action))},
                         {"workspace", rule.workspace}});
    }
    Json root = {{"version", kPlanVersion},
                 {"tiling", s.enabled},
                 {"mode", std::string(domain::toString(s.mode))},
                             {"layout", std::string(domain::toString(s.layout))},
                             {"workspaces", s.workspaces},
                             {"gap", s.gap},
                             {"outerGap", s.outerGap},
                             {"masterSize", s.masterPercent},
                             {"border", s.border},
                             {"borderColor", plan.border.toHex()},
                             {"runAsAdmin", s.runAsAdmin},
                             {"animations", s.animations},
                 {"keys", std::move(keys)},
                 {"rules", std::move(rules)}};
    if (plan.look)
    {
        root["look"] = lookJson(*plan.look);
    }
    return json::dumpPretty(root);
}

Result<Plan> parsePlan(std::string_view text)
{
    auto parsed = json::parse(text, json::ParseLimits{.maxBytes = kMaxPlanBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    if (!root.is_object() || intIn(root, "version", 0, 1000) != kPlanVersion)
    {
        return bad<Plan>("not a version 1 plan");
    }
    Plan plan;
    domain::TilingSettings& s = plan.settings;
    s.enabled = boolIn(root, "tiling").value_or(true); // a plan of before M14 always tiled
    const auto mode = textIn(root, "mode", 32);
    const auto layout = textIn(root, "layout", 32);
    const auto modeValue = mode ? domain::tilingModeFromName(*mode) : std::nullopt;
    const auto layoutValue = layout ? domain::tilingLayoutFromName(*layout) : std::nullopt;
    const auto workspaces = intIn(root, "workspaces", 1, domain::kMaxWorkspaces);
    const auto gap = intIn(root, "gap", 0, kMaxGap);
    const auto outerGap = intIn(root, "outerGap", 0, kMaxGap);
    const auto master = intIn(root, "masterSize", 10, 90);
    const auto border = boolIn(root, "border");
    const auto color = textIn(root, "borderColor", 16);
    std::optional<domain::Color> colorValue;
    if (color)
    {
        if (auto parsedColor = domain::Color::parse(*color))
        {
            colorValue = *parsedColor;
        }
    }
    const auto admin = boolIn(root, "runAsAdmin");
    if (!modeValue || !layoutValue || !workspaces || !gap || !outerGap || !master || !border || !colorValue ||
        !admin)
    {
        return bad<Plan>("a setting is missing or out of range");
    }
    s.mode = *modeValue;
    s.layout = *layoutValue;
    s.workspaces = static_cast<int>(*workspaces);
    s.gap = static_cast<int>(*gap);
    s.outerGap = static_cast<int>(*outerGap);
    s.masterPercent = static_cast<int>(*master);
    s.border = *border;
    s.runAsAdmin = *admin;
    s.animations = boolIn(root, "animations").value_or(true); // a plan of before v1.8 has none
    plan.border = *colorValue;

    const auto keys = root.find("keys");
    const auto rules = root.find("rules");
    if (keys == root.end() || !keys->is_array() || keys->size() > kMaxKeys || rules == root.end() ||
        !rules->is_array() || rules->size() > kMaxRules)
    {
        return bad<Plan>("keys and rules must be lists");
    }
    s.keys.clear();
    for (const auto& entry : *keys)
    {
        auto key = keyFrom(entry);
        if (!key)
        {
            return std::unexpected(std::move(key.error()));
        }
        s.keys.push_back(*key);
    }
    for (const auto& entry : *rules)
    {
        auto rule = ruleFrom(entry);
        if (!rule)
        {
            return std::unexpected(std::move(rule.error()));
        }
        s.rules.push_back(std::move(*rule));
    }
    if (const auto look = root.find("look"); look != root.end())
    {
        auto parsedLook = lookFrom(*look);
        if (!parsedLook)
        {
            return std::unexpected(std::move(parsedLook.error()));
        }
        plan.look = std::move(*parsedLook);
    }
    return plan;
}

LayoutOptions layoutOptions(const domain::TilingSettings& settings, double scale) noexcept
{
    if (scale < 0.5 || scale > 5.0 || std::isnan(scale))
    {
        scale = 1.0;
    }
    return LayoutOptions{static_cast<std::int32_t>(std::lround(settings.outerGap * scale)),
                         static_cast<std::int32_t>(std::lround(settings.gap * scale)),
                         settings.masterPercent / 100.0};
}

std::optional<KeyMatch> matchKey(std::span<const domain::TilingKey> keys, std::uint32_t modifiers,
                                 std::uint32_t vk) noexcept
{
    const int digit = digitOf(vk);
    for (const auto& key : keys)
    {
        if (key.keys.modifiers != modifiers)
        {
            continue;
        }
        if (key.anyDigit ? digit != 0 : key.keys.virtualKey == vk)
        {
            return KeyMatch{key.command, key.anyDigit ? digit : 0};
        }
    }
    return std::nullopt;
}

const domain::TilingRule* matchRule(std::span<const domain::TilingRule> rules, const WindowFacts& facts) noexcept
{
    for (const auto& rule : rules)
    {
        if (rule.app.empty() && rule.windowClass.empty() && rule.title.empty())
        {
            continue;
        }
        if ((rule.app.empty() || equalsIgnoreCase(rule.app, facts.app)) &&
            (rule.windowClass.empty() || rule.windowClass == facts.windowClass) &&
            (rule.title.empty() || containsIgnoreCase(facts.title, rule.title)))
        {
            return &rule;
        }
    }
    return nullptr;
}

std::string toJson(std::span<const Original> windows)
{
    Json list = Json::array();
    for (const auto& w : windows)
    {
        list.push_back({{"window", w.window},
                        {"process", w.process},
                        {"rect", {w.rect.left, w.rect.top, w.rect.right, w.rect.bottom}},
                        {"maximized", w.maximized},
                        {"hidden", w.hidden},
                        {"placed", w.placed}});
    }
    return json::dump({{"version", kPlanVersion}, {"windows", std::move(list)}});
}

Result<std::vector<Original>> parseOriginals(std::string_view text)
{
    auto parsed = json::parse(text, json::ParseLimits{.maxBytes = kMaxOriginalsBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    const auto windows = root.is_object() ? root.find("windows") : root.end();
    if (!root.is_object() || intIn(root, "version", 0, 1000) != kPlanVersion || windows == root.end() ||
        !windows->is_array() || windows->size() > kMaxOriginals)
    {
        return fail(ErrorCode::InvalidData, "tiler windows: not a version 1 list");
    }
    std::vector<Original> out;
    out.reserve(windows->size());
    for (const auto& entry : *windows)
    {
        const auto rect = entry.is_object() ? entry.find("rect") : entry.end();
        if (!entry.is_object() || rect == entry.end() || !rect->is_array() || rect->size() != 4 ||
            !std::ranges::all_of(*rect, [](const Json& v) {
                return v.is_number_integer() && v.get<std::int64_t>() >= -kMaxCoordinate &&
                       v.get<std::int64_t>() <= kMaxCoordinate;
            }))
        {
            return fail(ErrorCode::InvalidData, "tiler windows: a window is malformed");
        }
        const auto window = intIn(entry, "window", 1, std::int64_t{1} << 48);
        const auto process = intIn(entry, "process", 0, 0xFFFFFFFF);
        const auto maximized = boolIn(entry, "maximized");
        const auto hidden = boolIn(entry, "hidden");
        const auto placed = boolIn(entry, "placed");
        if (!window || !process || !maximized || !hidden || !placed)
        {
            return fail(ErrorCode::InvalidData, "tiler windows: a window is malformed");
        }
        out.push_back(Original{static_cast<WindowId>(*window), static_cast<std::uint32_t>(*process),
                               Rect{(*rect)[0].get<std::int32_t>(), (*rect)[1].get<std::int32_t>(),
                                    (*rect)[2].get<std::int32_t>(), (*rect)[3].get<std::int32_t>()},
                               *maximized, *hidden, *placed});
    }
    return out;
}

} // namespace rmw::tiling
