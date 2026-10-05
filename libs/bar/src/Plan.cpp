#include "rmw/bar/Plan.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/json/Value.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>

namespace rmw::bar
{
namespace
{

using json::Json;

Json colorJson(const std::optional<domain::ColorRef>& color)
{
    if (!color)
    {
        return nullptr;
    }
    const auto* resolved = std::get_if<domain::Color>(&color->value);
    return resolved != nullptr ? Json(resolved->toHex()) : Json(nullptr);
}

template <typename E>
std::optional<E> enumFromName(std::string_view name, std::initializer_list<E> values)
{
    for (const E value : values)
    {
        if (domain::toString(value) == name)
        {
            return value;
        }
    }
    return std::nullopt;
}

// Reads a plan written by the core; still checked like input (a bad plan must not crash the bar).
class PlanReader
{
public:
    Result<domain::BarConfig> bar(const Json& json, const std::string& path)
    {
        if (!json.is_object())
        {
            return bad(path, "expected an object");
        }
        domain::BarConfig bar;
        auto monitor = domain::MonitorSelector::parse(json::stringOr(json, "monitor", "all"));
        if (!monitor)
        {
            return bad(path + ".monitor", monitor.error().message());
        }
        bar.monitor = std::move(*monitor);
        const auto position = enumFromName<domain::BarPosition>(
            json::stringOr(json, "position", ""), {domain::BarPosition::Top, domain::BarPosition::Bottom,
                                                   domain::BarPosition::Left, domain::BarPosition::Right});
        const auto mode = enumFromName<domain::BarMode>(json::stringOr(json, "mode", ""),
                                                        {domain::BarMode::Reserve, domain::BarMode::Float});
        if (!position || !mode)
        {
            return bad(path, "unknown position or mode");
        }
        bar.position = *position;
        bar.mode = *mode;
        const auto thickness = json::intOr(json, "thickness", 0);
        const auto radius = json::intOr(json, "radius", 0);
        if (thickness < 16 || thickness > 512 || radius < 0 || radius > 256)
        {
            return bad(path, "thickness or radius out of range");
        }
        bar.thickness = static_cast<std::uint32_t>(thickness);
        bar.radius = static_cast<std::uint32_t>(radius);

        const Json length = json::memberOr(json, "length", Json::object());
        bar.length.percent = json::boolOr(length, "percent", true);
        const Json lengthValue = json::memberOr(length, "value", Json());
        if (!lengthValue.is_number())
        {
            return bad(path + ".length", "expected a number");
        }
        bar.length.value = lengthValue.get<double>();
        if (bar.length.percent ? (bar.length.value < 1 || bar.length.value > 100)
                               : (bar.length.value < 16 || bar.length.value > 16'384))
        {
            return bad(path + ".length", "out of range");
        }

        const Json margin = json::memberOr(json, "margin", Json::array());
        if (!margin.is_array() || margin.size() != 4)
        {
            return bad(path + ".margin", "expected 4 numbers");
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (!margin[i].is_number_integer() || margin[i].get<std::int64_t>() < 0 ||
                margin[i].get<std::int64_t>() > 1'024)
            {
                return bad(path + ".margin", "margins are integers from 0 to 1024");
            }
            bar.margin[i] = margin[i].get<std::uint32_t>();
        }

        const Json background = json::memberOr(json, "background", Json::object());
        const auto kind = enumFromName<domain::BarBackground::Kind>(
            json::stringOr(background, "kind", ""),
            {domain::BarBackground::Kind::Solid, domain::BarBackground::Kind::Transparent,
             domain::BarBackground::Kind::Acrylic, domain::BarBackground::Kind::Mica});
        if (!kind)
        {
            return bad(path + ".background", "unknown kind");
        }
        bar.background.kind = *kind;
        auto tint = color(background, "tint", path + ".background.tint");
        auto foreground = color(json, "foreground", path + ".foreground");
        if (!tint || !foreground)
        {
            return std::unexpected(!tint ? tint.error() : foreground.error());
        }
        bar.background.tint = *tint;
        bar.foreground = *foreground;
        const Json opacity = json::memberOr(background, "opacity", Json());
        if (opacity.is_number())
        {
            bar.background.opacity = std::clamp(opacity.get<double>(), 0.0, 1.0);
        }
        bar.autohide = json::boolOr(json, "autohide", false);
        bar.hideNativeTaskbar = json::boolOr(json, "hideNativeTaskbar", false);

        for (auto [key, zone] :
             {std::pair{"left", &bar.left}, std::pair{"center", &bar.center}, std::pair{"right", &bar.right}})
        {
            auto items = this->items(json::memberOr(json, key, Json::array()), path + "." + key);
            if (!items)
            {
                return std::unexpected(std::move(items.error()));
            }
            *zone = std::move(*items);
        }
        return bar;
    }

private:
    static Error badError(const std::string& path, std::string_view what)
    {
        return {ErrorCode::InvalidData, std::format("bar plan: {}: {}", path, what)};
    }

    static std::unexpected<Error> bad(const std::string& path, std::string_view what)
    {
        return std::unexpected(badError(path, what));
    }

    static Result<std::optional<domain::ColorRef>> color(const Json& object, std::string_view key,
                                                         const std::string& path)
    {
        const Json value = json::memberOr(object, key, Json());
        if (value.is_null())
        {
            return std::optional<domain::ColorRef>{};
        }
        auto parsed = value.is_string() ? domain::Color::parse(value.get<std::string>())
                                        : Result<domain::Color>(fail(ErrorCode::InvalidData, "not a color"));
        if (!parsed)
        {
            return bad(path, R"(expected "#RRGGBB" or "#RRGGBBAA")");
        }
        return std::optional<domain::ColorRef>(domain::ColorRef{*parsed});
    }

    static Result<std::vector<domain::BarItem>> items(const Json& array, const std::string& path)
    {
        if (!array.is_array() || array.size() > limits::kBarMaxItemsPerZone)
        {
            return bad(path, "expected a list of at most 32 items");
        }
        std::vector<domain::BarItem> items;
        for (const auto& entry : array)
        {
            const std::string id = json::stringOr(entry, "id", "");
            if (id.empty() || id.size() > 128)
            {
                return bad(path, "every item needs an id");
            }
            auto config = json::toValue(json::memberOr(entry, "config", Json()), 0);
            if (!config)
            {
                return bad(path, config.error().message());
            }
            items.push_back({id, std::move(*config)});
        }
        return items;
    }
};

} // namespace

Result<Plan> planFromRice(const domain::Rice& rice)
{
    Plan plan;
    plan.riceName = rice.meta.name;
    const auto resolve = [&](std::optional<domain::ColorRef>& color) -> Result<void> {
        if (!color || !color->isPalette())
        {
            return {};
        }
        const auto& name = std::get<std::string>(color->value);
        const auto entry =
            std::ranges::find_if(rice.palette, [&](const domain::PaletteEntry& e) { return e.name == name; });
        if (entry == rice.palette.end())
        {
            color.reset(); // from a skin: this rice has no such color
            return {};
        }
        color->value = entry->color;
        return {};
    };
    for (domain::BarConfig bar : rice.bars)
    {
        if (auto tint = resolve(bar.background.tint); !tint)
        {
            return std::unexpected(std::move(tint.error()));
        }
        if (auto foreground = resolve(bar.foreground); !foreground)
        {
            return std::unexpected(std::move(foreground.error()));
        }
        plan.bars.push_back(std::move(bar));
    }
    return plan;
}

std::string toJson(const Plan& plan)
{
    Json bars = Json::array();
    for (const auto& bar : plan.bars)
    {
        Json zones = Json::object();
        for (auto [key, zone] :
             {std::pair{"left", &bar.left}, std::pair{"center", &bar.center}, std::pair{"right", &bar.right}})
        {
            Json items = Json::array();
            for (const auto& item : *zone)
            {
                items.push_back({{"id", item.id}, {"config", json::fromValue(item.config)}});
            }
            zones[key] = std::move(items);
        }
        bars.push_back(
            {{"monitor", bar.monitor.toString()},
             {"position", domain::toString(bar.position)},
             {"mode", domain::toString(bar.mode)},
             {"thickness", bar.thickness},
             {"length", {{"percent", bar.length.percent}, {"value", bar.length.value}}},
             {"margin", bar.margin},
             {"radius", bar.radius},
             {"background",
              {{"kind", domain::toString(bar.background.kind)},
               {"tint", colorJson(bar.background.tint)},
               {"opacity", bar.background.opacity ? Json(*bar.background.opacity) : Json(nullptr)}}},
             {"foreground", colorJson(bar.foreground)},
             {"autohide", bar.autohide},
             {"hideNativeTaskbar", bar.hideNativeTaskbar},
             {"left", zones["left"]},
             {"center", zones["center"]},
             {"right", zones["right"]}});
    }
    Json plugins = Json::array();
    for (const auto& plugin : plan.plugins)
    {
        plugins.push_back(
            {{"id", plugin.id}, {"folder", utf16To8Lossy(plugin.folder.native())}, {"dev", plugin.dev}});
    }
    return json::dumpPretty({{"version", kPlanVersion},
                             {"rice", plan.riceName},
                             {"bars", std::move(bars)},
                             {"plugins", std::move(plugins)}});
}

Result<Plan> planFromJson(std::string_view text)
{
    auto parsed = json::parse(text, {.maxBytes = limits::kManifestMaxBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()).withContext("bar plan"));
    }
    const Json& root = *parsed;
    if (!root.is_object() || json::intOr(root, "version", 0) != kPlanVersion)
    {
        return fail(ErrorCode::Unsupported, std::format("bar plan: expected version {}", kPlanVersion));
    }
    const Json bars = json::memberOr(root, "bars", Json());
    if (!bars.is_array() || bars.size() > limits::kRiceMaxBars)
    {
        return fail(ErrorCode::InvalidData, "bar plan: expected a list of at most 16 bars");
    }
    Plan plan;
    plan.riceName = json::stringOr(root, "rice", "");
    PlanReader reader;
    for (std::size_t i = 0; i < bars.size(); ++i)
    {
        auto bar = reader.bar(bars[i], std::format("bars[{}]", i));
        if (!bar)
        {
            return std::unexpected(std::move(bar.error()));
        }
        plan.bars.push_back(std::move(*bar));
    }
    const Json plugins = json::memberOr(root, "plugins", Json::array());
    if (!plugins.is_array() || plugins.size() > limits::kRiceMaxPluginRequirements)
    {
        return fail(ErrorCode::InvalidData, "bar plan: plugins: expected a short list");
    }
    for (const auto& entry : plugins)
    {
        PlanPlugin plugin;
        plugin.id = json::stringOr(entry, "id", "");
        auto folder = utf8To16(json::stringOr(entry, "folder", ""));
        if (!domain::isValidPackageId(plugin.id) || !folder || folder->empty())
        {
            return fail(ErrorCode::InvalidData, "bar plan: plugins: invalid entry");
        }
        plugin.folder = std::filesystem::path(std::move(*folder));
        plugin.dev = json::boolOr(entry, "dev", false);
        if (!plugin.folder.is_absolute())
        {
            return fail(ErrorCode::InvalidData, "bar plan: plugins: the folder must be absolute");
        }
        plan.plugins.push_back(std::move(plugin));
    }
    return plan;
}

bool hidesNativeTaskbar(const Plan& plan) noexcept
{
    return std::ranges::any_of(plan.bars, [](const domain::BarConfig& bar) { return bar.hideNativeTaskbar; });
}

} // namespace rmw::bar
