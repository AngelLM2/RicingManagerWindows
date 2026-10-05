#include "rmw/domain/MonitorSelector.hpp"

#include <array>
#include <format>
#include <optional>

namespace rmw::domain
{
namespace
{

constexpr std::string_view kEdidPrefix = "edid:";
constexpr std::size_t kEdidKeyMaxBytes = 256;

bool isPrintableAscii(std::string_view text) noexcept
{
    for (const char c : text)
    {
        if (c < 0x21 || c > 0x7E)
        {
            return false;
        }
    }
    return true;
}

// "Smaller is better" ordering for positional selectors: main axis, then the other axis, then
// the primary monitor first.
struct ExtremeKey
{
    std::int64_t main;
    std::int64_t tie;
    bool notPrimary;

    friend auto operator<=>(const ExtremeKey&, const ExtremeKey&) = default;
};

ExtremeKey extremeKey(MonitorSelector::Kind kind, const MonitorFacts& m) noexcept
{
    switch (kind)
    {
    case MonitorSelector::Kind::Left:
        return {m.left, m.top, !m.primary};
    case MonitorSelector::Kind::Right:
        return {-std::int64_t{m.right}, m.top, !m.primary};
    case MonitorSelector::Kind::Top:
        return {m.top, m.left, !m.primary};
    default:
        return {-std::int64_t{m.bottom}, m.left, !m.primary};
    }
}

// The extreme monitor for a positional selector; nullopt when there are no monitors.
std::optional<std::size_t> pickExtreme(MonitorSelector::Kind kind, std::span<const MonitorFacts> monitors)
{
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (!best || extremeKey(kind, monitors[i]) < extremeKey(kind, monitors[*best]))
        {
            best = i;
        }
    }
    return best;
}

} // namespace

Result<MonitorSelector> MonitorSelector::parse(std::string_view text)
{
    struct Named
    {
        std::string_view name;
        Kind kind;
    };
    static constexpr std::array<Named, 6> named = {{
        {"primary", Kind::Primary},
        {"all", Kind::All},
        {"left", Kind::Left},
        {"right", Kind::Right},
        {"top", Kind::Top},
        {"bottom", Kind::Bottom},
    }};
    for (const auto& entry : named)
    {
        if (text == entry.name)
        {
            return MonitorSelector{entry.kind, {}};
        }
    }
    if (text.starts_with(kEdidPrefix))
    {
        const std::string_view key = text.substr(kEdidPrefix.size());
        if (!key.empty() && key.size() <= kEdidKeyMaxBytes && isPrintableAscii(key))
        {
            return MonitorSelector{Kind::Edid, std::string(key)};
        }
    }
    return fail(
        ErrorCode::InvalidData,
        std::format("invalid monitor selector '{}' (expected primary, all, left, right, top, bottom or "
                    "edid:<id>)",
                    text));
}

std::string MonitorSelector::toString() const
{
    switch (kind)
    {
    case Kind::Primary:
        return "primary";
    case Kind::All:
        return "all";
    case Kind::Left:
        return "left";
    case Kind::Right:
        return "right";
    case Kind::Top:
        return "top";
    case Kind::Bottom:
        return "bottom";
    case Kind::Edid:
        return std::string(kEdidPrefix) + edidKey;
    }
    return "all";
}

std::vector<int> resolveMonitorRules(std::span<const MonitorSelector> rules,
                                     std::span<const MonitorFacts> monitors)
{
    std::vector<int> owner(monitors.size(), -1);
    const auto claim = [&](std::size_t monitor, std::size_t rule) {
        if (owner[monitor] < 0)
        {
            owner[monitor] = static_cast<int>(rule);
        }
    };

    for (std::size_t rule = 0; rule < rules.size(); ++rule)
    {
        const MonitorSelector& selector = rules[rule];
        switch (selector.kind)
        {
        case MonitorSelector::Kind::All:
            break; // handled after every specific rule
        case MonitorSelector::Kind::Primary:
            for (std::size_t m = 0; m < monitors.size(); ++m)
            {
                if (monitors[m].primary)
                {
                    claim(m, rule);
                }
            }
            break;
        case MonitorSelector::Kind::Edid:
            for (std::size_t m = 0; m < monitors.size(); ++m)
            {
                if (monitors[m].identityKey == selector.edidKey)
                {
                    claim(m, rule);
                }
            }
            break;
        default:
            if (const auto extreme = pickExtreme(selector.kind, monitors))
            {
                claim(*extreme, rule);
            }
            break;
        }
    }

    for (std::size_t rule = 0; rule < rules.size(); ++rule)
    {
        if (rules[rule].kind == MonitorSelector::Kind::All)
        {
            for (std::size_t m = 0; m < monitors.size(); ++m)
            {
                claim(m, rule);
            }
            break; // a second "all" can never match anything
        }
    }
    return owner;
}

} // namespace rmw::domain
