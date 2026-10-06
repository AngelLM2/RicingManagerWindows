#include "rmw/modules/Appearance.hpp"

#include "rmw/base/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
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
constexpr std::string_view kThemes = R"(Software\Microsoft\Windows\CurrentVersion\Themes)";
constexpr std::string_view kOurTheme = "ricing-manager"; // "theme" from a rice: the managed theme
constexpr std::string_view kWindowsTheme = "windows";      // the theme Windows ships for the mode
constexpr auto kThemeSettleStep = std::chrono::milliseconds(100);

// RicingManager.theme, RicingManager-2.theme: the managed theme (ours).
bool isOurTheme(const Json& value)
{
    if (!value.is_string())
    {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text == kOurTheme)
    {
        return true;
    }
    const auto slash = text.find_last_of("\\/");
    std::string name = text.substr(slash == std::string::npos ? 0 : slash + 1);
    std::ranges::transform(name, name.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return name.starts_with("ricingmanager");
}

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

void AppearanceLogic::forOriginal(Json& state) const
{
    // A theme of ours as the original: the snapshot was taken over an earlier install's rice. What
    // Windows ships comes back instead.
    if (const auto theme = state.find("theme"); theme != state.end() && isOurTheme(*theme))
    {
        *theme = kWindowsTheme;
    }
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
    if (desired.contains("appsLight") || desired.contains("accent"))
    {
        desired["theme"] = kOurTheme; // the look goes through our managed theme
    }
    return desired;
}

bool AppearanceLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    if (field == "accent")
    {
        return colorizationFromHex(a) == colorizationFromHex(b);
    }
    if (field == "theme")
    {
        if (isOurTheme(a) || isOurTheme(b))
        {
            return isOurTheme(a) && isOurTheme(b);
        }
        if (a.is_string() && b.is_string())
        {
            const auto lower = [](std::string text) {
                std::ranges::transform(text, text.begin(), [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                });
                return text;
            };
            return lower(a.get<std::string>()) == lower(b.get<std::string>());
        }
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
    auto theme = m_registry.read(kThemes, "CurrentTheme");
    if (!theme)
    {
        return std::unexpected(std::move(theme.error()));
    }
    result.state["theme"] = *theme && ((*theme)->type == ports::RegistryValue::Type::String ||
                                       (*theme)->type == ports::RegistryValue::Type::ExpandString)
                                ? Json((*theme)->text)
                                : Json(nullptr);
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

    // A restore: the theme the user had selected comes back first (its look only; the pointers and the
    // recent colors stay as they are). The fields below then set what it does not. A theme that is gone
    // (or "windows") is the one Windows ships for the mode.
    bool themeApplied = false;
    std::string themeFile;
    if (const auto theme = desired.find("theme"); theme != desired.end() && theme->is_string() && !isOurTheme(*theme))
    {
        const auto& asked = theme->get_ref<const std::string&>();
        std::vector<std::string> candidates;
        if (asked != kWindowsTheme)
        {
            candidates.push_back(asked);
        }
        if (asked == kWindowsTheme || !m_shell.fileExists(asked))
        {
            auto light = readDword(m_registry, kPersonalize, "SystemUsesLightTheme", 1);
            candidates.push_back(m_theme.windowsThemeFile(!systemLight.value_or(light && *light != 0)));
        }
        for (const auto& file : candidates)
        {
            auto applied = m_theme.applyThemeFile(file);
            if (applied)
            {
                themeApplied = true;
                themeFile = file;
                break;
            }
            log::warn("appearance: the theme {} cannot come back ({})", file, applied.error().toString());
        }
        if (!themeApplied)
        {
            log::warn("appearance: no theme came back; the look is set instead");
        }
    }

    if (appsLight || systemLight || colorization)
    {
        const auto rgb = [](std::optional<std::uint32_t> argb) {
            return argb ? std::optional<std::uint32_t>(*argb & 0x00FFFFFF) : std::nullopt;
        };
        const auto shows = [&](const Json& state) {
            return (!appsLight || state["appsLight"] == *appsLight) &&
                   (!systemLight || state["systemLight"] == *systemLight) &&
                   (!colorization || rgb(colorizationFromHex(state["accent"])) == rgb(colorization));
        };
        auto current = capture();
        if (!current)
        {
            return std::unexpected(std::move(current.error()));
        }
        // The user's theme names itself first; its mode and accent reach the registry a moment later.
        const auto settleUntil = std::chrono::steady_clock::now() + m_themeSettle;
        while (themeApplied && !shows(current->state) && std::chrono::steady_clock::now() < settleUntil)
        {
            std::this_thread::sleep_for(kThemeSettleStep);
            current = capture();
            if (!current)
            {
                return std::unexpected(std::move(current.error()));
            }
        }
        const Json& now = current->state;
        const bool shown = themeApplied && shows(now);
        if (themeApplied && !shown)
        {
            // Windows does not always take a theme's accent with its look (seen in the Sandbox): the look
            // the user had goes on through our theme, and their theme is named the current one again.
            log::warn("appearance: the theme the user had does not show its look (light apps {}, system {}, "
                      "accent {}); setting it, then naming their theme again",
                      now["appsLight"].dump(), now["systemLight"].dump(), now["accent"].dump());
        }
        if (!shown)
        {
            const ports::ThemeLook look{
                appsLight.value_or(now["appsLight"].get<bool>()),
                systemLight.value_or(now["systemLight"].get<bool>()),
                colorization.value_or(colorizationFromHex(now["accent"]).value_or(kDefaultColorization)),
            };
            auto themed = m_theme.applyLook(look);
            if (!themed)
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
    }

    // The user's theme is the current one (and never one of ours the one to come back to after high
    // contrast), whichever way its look came back: Windows writes these in its own time, or not at all
    // after our fallback.
    if (themeApplied)
    {
        if (auto named = m_theme.nameCurrentTheme(themeFile); !named)
        {
            log::warn("appearance: cannot name {} the current theme: {}", themeFile, named.error().toString());
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
