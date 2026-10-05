#include "rmw/modules/Appearance.hpp"

#include "rmw/base/Log.hpp"

#include <array>
#include <ctime>
#include <format>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr std::string_view kPersonalize = R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
constexpr std::string_view kDwm = R"(Software\Microsoft\Windows\DWM)";
constexpr std::uint32_t kDefaultColorization = 0xC40078D4; // Windows blue
constexpr std::uint32_t kColorizationAlpha = 0xC4000000;   // what Windows writes for opaque accents

domain::TimeOfDay localNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) != 0)
    {
        return {12, 0};
    }
    return {static_cast<std::uint8_t>(local.tm_hour), static_cast<std::uint8_t>(local.tm_min)};
}

Result<std::uint32_t> readDword(ports::IRegistry& registry, std::string_view key, std::string_view name,
                                std::uint32_t fallback)
{
    auto value = registry.read(key, name);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!*value || (*value)->type != ports::RegistryValue::Type::Dword)
    {
        return fallback;
    }
    return (*value)->dword;
}

std::string accentHex(std::uint32_t argb)
{
    return std::format("#{:06X}", argb & 0x00FFFFFF);
}

// "#RRGGBB" -> 0xC4RRGGBB
std::optional<std::uint32_t> colorizationFromHex(const Json& value)
{
    if (!value.is_string())
    {
        return std::nullopt;
    }
    const auto color = domain::Color::parse(value.get_ref<const std::string&>());
    if (!color)
    {
        return std::nullopt;
    }
    return kColorizationAlpha | (std::uint32_t{color->r} << 16) | (std::uint32_t{color->g} << 8) | color->b;
}

Result<bool> optionalBool(const Json& desired, std::string_view field, std::optional<bool>& out)
{
    const auto value = desired.find(field);
    if (value == desired.end())
    {
        return false;
    }
    if (!value->is_boolean())
    {
        return fail(ErrorCode::InvalidArgument, std::format("field '{}' needs true or false", field));
    }
    out = value->get<bool>();
    return true;
}

} // namespace

bool isLightPeriod(const domain::AppearanceSchedule& schedule, domain::TimeOfDay now) noexcept
{
    if (schedule.lightAt < schedule.darkAt)
    {
        return schedule.lightAt <= now && now < schedule.darkAt;
    }
    return now >= schedule.lightAt || now < schedule.darkAt; // light period crosses midnight
}

AppearanceLogic::AppearanceLogic(std::function<domain::TimeOfDay()> now)
    : m_now(now ? std::move(now) : localNow)
{
}

Result<Json> AppearanceLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& /*assets*/) const
{
    Json desired = Json::object();
    if (!rice.appearance)
    {
        return desired;
    }
    const domain::AppearanceConfig& appearance = *rice.appearance;
    if (appearance.mode)
    {
        std::optional<bool> light;
        switch (*appearance.mode)
        {
        case domain::ThemeMode::Light:
            light = true;
            break;
        case domain::ThemeMode::Dark:
            light = false;
            break;
        case domain::ThemeMode::Schedule:
            light = isLightPeriod(appearance.schedule.value_or(domain::AppearanceSchedule{}), m_now());
            break;
        case domain::ThemeMode::System:
            break; // the user's own choice stays
        }
        if (light)
        {
            desired["appsLight"] = *light;
            desired["systemLight"] = *light;
        }
    }
    if (appearance.accent)
    {
        desired["accent"] = std::format("#{:02X}{:02X}{:02X}", appearance.accent->r, appearance.accent->g,
                                        appearance.accent->b);
    }
    if (appearance.transparency)
    {
        desired["transparency"] = *appearance.transparency;
    }
    if (appearance.accentOnTaskbar)
    {
        desired["accentOnTaskbar"] = *appearance.accentOnTaskbar;
    }
    return desired;
}

bool AppearanceLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    if (field == "accent")
    {
        return colorizationFromHex(a) == colorizationFromHex(b);
    }
    return a == b;
}

Result<engine::CaptureResult> AppearanceSystem::capture()
{
    struct Read
    {
        std::string_view field;
        std::string_view key;
        std::string_view name;
        std::uint32_t fallback; // what Windows assumes when the value is missing
    };
    static constexpr std::array<Read, 4> flags = {{
        {"appsLight", kPersonalize, "AppsUseLightTheme", 1},
        {"systemLight", kPersonalize, "SystemUsesLightTheme", 1},
        {"transparency", kPersonalize, "EnableTransparency", 1},
        {"accentOnTaskbar", kPersonalize, "ColorPrevalence", 0},
    }};

    engine::CaptureResult result;
    for (const auto& flag : flags)
    {
        auto value = readDword(m_registry, flag.key, flag.name, flag.fallback);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[std::string(flag.field)] = *value != 0;
    }
    auto colorization = readDword(m_registry, kDwm, "ColorizationColor", kDefaultColorization);
    if (!colorization)
    {
        return std::unexpected(std::move(colorization.error()));
    }
    result.state["accent"] = accentHex(*colorization);
    return result;
}

Result<void> AppearanceSystem::apply(const Json& desired)
{
    std::optional<bool> appsLight;
    std::optional<bool> systemLight;
    std::optional<bool> transparency;
    std::optional<bool> accentOnTaskbar;
    for (auto [field, out] :
         {std::pair{"appsLight", &appsLight}, std::pair{"systemLight", &systemLight},
          std::pair{"transparency", &transparency}, std::pair{"accentOnTaskbar", &accentOnTaskbar}})
    {
        if (auto read = optionalBool(desired, field, *out); !read)
        {
            return std::unexpected(std::move(read.error()));
        }
    }
    std::optional<std::uint32_t> colorization;
    if (const auto accent = desired.find("accent"); accent != desired.end())
    {
        colorization = colorizationFromHex(*accent);
        if (!colorization)
        {
            return fail(ErrorCode::InvalidArgument, "field 'accent' needs \"#RRGGBB\"");
        }
    }

    if (appsLight || systemLight || colorization)
    {
        auto current = capture();
        if (!current)
        {
            return std::unexpected(std::move(current.error()));
        }
        const Json& now = current->state;
        const ports::ThemeLook look{
            appsLight.value_or(now["appsLight"].get<bool>()),
            systemLight.value_or(now["systemLight"].get<bool>()),
            colorization.value_or(colorizationFromHex(now["accent"]).value_or(kDefaultColorization)),
        };
        if (auto themed = m_theme.applyLook(look); !themed)
        {
            const bool accentChanges =
                colorization && accentHex(*colorization) != now["accent"].get<std::string>();
            if (accentChanges)
            {
                return std::unexpected(std::move(themed.error()).withContext("apply accent color"));
            }
            // Mode only: the registry fallback works, though the taskbar may not repaint until
            // the next sign-in (ADR-0005).
            log::warn("theme apply failed ({}); falling back to the registry for light/dark",
                      themed.error().toString());
            for (auto [name, light] : {std::pair{"AppsUseLightTheme", look.appsLight},
                                       std::pair{"SystemUsesLightTheme", look.systemLight}})
            {
                if (auto written =
                        m_registry.write(kPersonalize, name, ports::RegistryValue::fromDword(light ? 1 : 0));
                    !written)
                {
                    return written;
                }
            }
            if (auto broadcast = m_shell.broadcastSettingChange("ImmersiveColorSet"); !broadcast)
            {
                return broadcast;
            }
        }
    }

    bool flagsChanged = false;
    for (auto [name, value] :
         {std::pair{"EnableTransparency", transparency}, std::pair{"ColorPrevalence", accentOnTaskbar}})
    {
        if (!value)
        {
            continue;
        }
        if (auto written =
                m_registry.write(kPersonalize, name, ports::RegistryValue::fromDword(*value ? 1 : 0));
            !written)
        {
            return written;
        }
        flagsChanged = true;
    }
    if (flagsChanged)
    {
        return m_shell.broadcastSettingChange("ImmersiveColorSet");
    }
    return {};
}

} // namespace rmw::modules
