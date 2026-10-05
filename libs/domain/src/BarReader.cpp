#include "BarReader.hpp"

#include "EnumNames.hpp"

#include "rmw/domain/Identifiers.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace rmw::domain::detail
{
namespace
{

constexpr std::string_view kPalettePrefix = "palette.";

// The keys a bar table can hold besides "monitor" and "skin" (what a skin can set).
constexpr auto kBarKeys = std::to_array<std::string_view>({
    "position",
    "mode",
    "thickness",
    "length",
    "margin",
    "radius",
    "background",
    "foreground",
    "autohide",
    "hide_native_taskbar",
    "left",
    "center",
    "right",
});

} // namespace

std::optional<ColorRef> readColorRef(TableReader& t, std::string_view key, const Rice* rice)
{
    const auto text = t.string(key);
    if (!text)
    {
        return std::nullopt;
    }
    if (text->starts_with(kPalettePrefix))
    {
        std::string name = text->substr(kPalettePrefix.size());
        if (!isValidPaletteName(name))
        {
            t.error(key, std::format("invalid palette name '{}'", name));
            return std::nullopt;
        }
        if (name != "accent" && rice != nullptr && rice->findPalette(name) == nullptr)
        {
            t.error(key, std::format("'{}' is not defined in [palette]", *text));
            return std::nullopt;
        }
        return ColorRef{std::move(name)};
    }
    auto color = Color::parse(*text);
    if (!color)
    {
        t.error(key, color.error().message() + " — or use palette.<name>");
        return std::nullopt;
    }
    return ColorRef{*color};
}

BarTableReader::BarTableReader(Diagnostics& diagnostics, const Rice* rice)
    : m_diagnostics(diagnostics)
    , m_rice(rice)
{
}

void BarTableReader::read(const toml::table& table, const std::string& path, BarConfig& bar, bool skin)
{
    TableReader t(table, path, m_diagnostics);
    if (!skin)
    {
        if (auto monitor = t.parsed("monitor", MonitorSelector::parse))
        {
            bar.monitor = std::move(*monitor);
        }
        if (auto id = t.string("skin"))
        {
            if (!isValidPackageId(*id))
            {
                t.error("skin", std::format("invalid plugin id '{}'", *id));
            }
            else
            {
                if (!requiresPlugin(*id))
                {
                    t.warning("skin",
                              std::format("plugin '{}' is not listed in [plugins] requires; users will "
                                          "not be asked to install it",
                                          *id));
                }
                bar.skin = std::move(*id);
            }
        }
    }
    bar.position = t.enumeration<BarPosition>("position", kBarPositionNames).value_or(bar.position);
    bar.mode = t.enumeration<BarMode>("mode", kBarModeNames).value_or(bar.mode);
    if (const auto thickness = t.integer("thickness", 16, 512))
    {
        bar.thickness = static_cast<std::uint32_t>(*thickness);
    }
    readBarLength(t, bar);
    readBarMargin(t, bar);
    if (const auto radius = t.integer("radius", 0, 256))
    {
        bar.radius = static_cast<std::uint32_t>(*radius);
    }
    readBarBackground(t, bar);
    if (auto foreground = readColorRef(t, "foreground", m_rice))
    {
        bar.foreground = std::move(foreground);
    }
    bar.autohide = t.boolean("autohide").value_or(bar.autohide);
    bar.hideNativeTaskbar = t.boolean("hide_native_taskbar").value_or(bar.hideNativeTaskbar);
    readBarZone(t, "left", bar.left);
    readBarZone(t, "center", bar.center);
    readBarZone(t, "right", bar.right);
    t.reportUnknownKeys();
    for (const std::string_view key : kBarKeys)
    {
        if (table.contains(key))
        {
            bar.keys.emplace_back(key);
        }
    }
}

void BarTableReader::readBarLength(TableReader& t, BarConfig& bar)
{
    const toml::node* node = t.get("length");
    if (node == nullptr)
    {
        return;
    }
    if (const auto* pixels = node->as_integer())
    {
        if (pixels->get() < 16 || pixels->get() > 16'384)
        {
            t.error("length", std::format("{} px is out of range (16..16384)", pixels->get()));
            return;
        }
        bar.length = {false, static_cast<double>(pixels->get())};
        return;
    }
    if (const auto* text = node->as_string())
    {
        const std::string& value = text->get();
        double percent = 0.0;
        if (value.size() >= 2 && value.back() == '%')
        {
            const char* first = value.data();
            const char* last = value.data() + value.size() - 1;
            const auto [ptr, ec] = std::from_chars(first, last, percent, std::chars_format::fixed);
            if (ec == std::errc{} && ptr == last && percent >= 1.0 && percent <= 100.0)
            {
                bar.length = {true, percent};
                return;
            }
        }
    }
    t.error("length", "expected a percentage like \"100%\" (1–100) or a size in pixels like 600");
}

void BarTableReader::readBarMargin(TableReader& t, BarConfig& bar)
{
    constexpr std::int64_t kMax = 1'024;
    const toml::node* node = t.get("margin");
    if (node == nullptr)
    {
        return;
    }
    const auto checked = [&](const toml::node& item, std::uint32_t& out) {
        const auto* value = item.as_integer();
        if (value == nullptr || value->get() < 0 || value->get() > kMax)
        {
            t.error("margin", std::format("margins are integers from 0 to {}", kMax));
            return false;
        }
        out = static_cast<std::uint32_t>(value->get());
        return true;
    };

    std::array<std::uint32_t, 4> values{};
    if (node->is_integer())
    {
        if (checked(*node, values[0]))
        {
            std::ranges::fill(bar.margin, values[0]);
        }
        return;
    }
    const auto* array = node->as_array();
    if (array == nullptr || (array->size() != 2 && array->size() != 4))
    {
        t.error("margin", "expected one number, [vertical, horizontal] or [top, right, bottom, left]");
        return;
    }
    for (std::size_t i = 0; i < array->size(); ++i)
    {
        if (!checked(*array->get(i), values[i]))
        {
            return;
        }
    }
    if (array->size() == 2)
    {
        bar.margin[0] = bar.margin[2] = values[0];
        bar.margin[1] = bar.margin[3] = values[1];
    }
    else
    {
        bar.margin = values;
    }
}

void BarTableReader::readBarBackground(TableReader& t, BarConfig& bar)
{
    const toml::node* node = t.get("background");
    if (node == nullptr)
    {
        return;
    }
    if (node->is_string())
    {
        // Shorthand: background = "acrylic"
        if (const auto kind = t.enumeration<BarBackground::Kind>("background", kBarBackgroundNames))
        {
            bar.background.kind = *kind;
        }
        return;
    }
    const auto* table = node->as_table();
    if (table == nullptr)
    {
        t.error("background", std::format("expected a kind ({}) or a table {{ kind, tint, opacity }}",
                                          enumChoices<BarBackground::Kind>(kBarBackgroundNames)));
        return;
    }
    TableReader b(*table, t.pathOf("background"), m_diagnostics);
    bar.background.kind =
        b.enumeration<BarBackground::Kind>("kind", kBarBackgroundNames).value_or(BarBackground::Kind::Solid);
    bar.background.tint = readColorRef(b, "tint", m_rice);
    bar.background.opacity = b.number("opacity", 0.0, 1.0);
    b.reportUnknownKeys();
}

void BarTableReader::readBarZone(TableReader& t, std::string_view key, std::vector<BarItem>& items)
{
    const auto* array = t.array(key, limits::kBarMaxItemsPerZone);
    if (array == nullptr)
    {
        return;
    }
    items.clear();
    std::size_t index = 0;
    for (const auto& node : *array)
    {
        const std::string path = indexPath(t.pathOf(key), index++);
        BarItem item;
        if (const auto* id = node.as_string())
        {
            item.id = id->get();
        }
        else if (const auto* table = node.as_table())
        {
            // { id = "clock", format = "%H:%M" }: everything but the id is the widget's config.
            const toml::node* idNode = table->get("id");
            if (idNode == nullptr || !idNode->is_string())
            {
                errorAt(m_diagnostics, node, path, "a widget table needs id = \"<widget>\"");
                continue;
            }
            item.id = idNode->as_string()->get();
            Value::Table config;
            std::size_t budget = limits::kConfigValueMaxElements;
            bool ok = true;
            for (const auto& [childKey, child] : *table)
            {
                if (childKey.str() == "id")
                {
                    continue;
                }
                auto value = toValue(child, childPath(path, childKey.str()), m_diagnostics, budget);
                if (!value)
                {
                    ok = false;
                    break;
                }
                config.push_back({std::string(childKey.str()), std::move(*value)});
            }
            if (!ok)
            {
                continue;
            }
            item.config.data = std::move(config);
        }
        else
        {
            errorAt(m_diagnostics, node, path,
                    std::format("expected a widget name or {{ id = ... }}, found {}", typeName(node)));
            continue;
        }

        if (item.isPlugin() ? !isValidPackageId(item.id) : !isValidBuiltinName(item.id))
        {
            errorAt(m_diagnostics, node, path, std::format("invalid widget name '{}'", item.id));
            continue;
        }
        if (item.isPlugin() && m_rice != nullptr && !requiresPlugin(item.id))
        {
            warningAt(
                m_diagnostics, node, path,
                std::format("plugin '{}' is not listed in [plugins] requires; users will not be asked to "
                            "install it",
                            item.id));
        }
        items.push_back(std::move(item));
    }
}

bool BarTableReader::requiresPlugin(std::string_view id) const noexcept
{
    return m_rice != nullptr && std::ranges::any_of(m_rice->pluginRequirements,
                                                    [id](const PluginRequirement& r) { return r.id == id; });
}

std::span<const std::string_view> barKeys() noexcept
{
    return kBarKeys;
}

} // namespace rmw::domain::detail
