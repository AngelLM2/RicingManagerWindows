#include "rmw/modules/Sounds.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/SoundFiles.hpp"
#include "rmw/modules/RegistryFields.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr std::string_view kSchemes = R"(AppEvents\Schemes)";
constexpr std::array<std::string_view, 1> kDependsOn = {"appearance"}; // a .theme can carry sounds

// The key whose default value is the event's file.
std::string currentKey(domain::SoundEvent event)
{
    return std::format(R"({}\{}\.Current)", kSchemes, domain::registryKey(event));
}

Json stringField(const std::optional<ports::RegistryValue>& value)
{
    if (!value)
    {
        return nullptr;
    }
    if (value->type != ports::RegistryValue::Type::String && value->type != ports::RegistryValue::Type::ExpandString)
    {
        return rawField(*value); // not a path: kept as it is, a restore writes it back
    }
    return Json{{"type", value->type == ports::RegistryValue::Type::String ? "string" : "expand"},
                {"data", value->text}};
}

Result<void> writeStringField(ports::IRegistry& registry, ports::IShell& shell, const std::string& key,
                              std::string_view field, const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(key, "");
    }
    if (isRawField(value))
    {
        return writeRawField(registry, key, "", value);
    }
    const auto type = value.find("type");
    const auto data = value.find("data");
    if (!value.is_object() || type == value.end() || data == value.end() || !type->is_string() ||
        !data->is_string())
    {
        return fail(ErrorCode::InvalidArgument, std::format("sound field '{}' is malformed", field));
    }
    // The file the user had while it is there, else its copy in the store.
    const std::string copy = json::stringOr(value, "copy", "");
    const std::string& text =
        copy.empty() || shell.fileExists(data->get_ref<const std::string&>()) ? data->get_ref<const std::string&>() : copy;
    return registry.write(key, "",
                          *type == "string" ? ports::RegistryValue::fromString(text)
                                            : ports::RegistryValue::fromExpandString(text));
}

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(text[i]) != lower(prefix[i]))
        {
            return false;
        }
    }
    return true;
}

Json silence()
{
    return Json{{"type", "string"}, {"data", ""}};
}

// "others": every event beyond the eighteen a rice names (Explorer's navigation click, an app's own...).
constexpr std::string_view kOthers = "others";
constexpr std::string_view kSilent = "silent"; // a rice's "no sounds": every one of them, whatever is there

bool known(std::string_view relative)
{
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        if (startsWithIgnoreCase(relative, domain::registryKey(static_cast<domain::SoundEvent>(i))) &&
            relative.size() == domain::registryKey(static_cast<domain::SoundEvent>(i)).size())
        {
            return true;
        }
    }
    return false;
}

// "Apps\<app>\<event>" -> its sound, for every event Windows knows but the eighteen.
Result<Json> otherEvents(ports::IRegistry& registry)
{
    Json others = Json::object();
    const std::string apps = std::format(R"({}\Apps)", kSchemes);
    auto names = registry.subkeys(apps);
    if (!names)
    {
        return std::unexpected(std::move(names.error()));
    }
    for (const auto& app : *names)
    {
        auto events = registry.subkeys(std::format(R"({}\{})", apps, app));
        if (!events)
        {
            return std::unexpected(std::move(events.error()));
        }
        for (const auto& event : *events)
        {
            const std::string relative = std::format(R"(Apps\{}\{})", app, event);
            if (known(relative))
            {
                continue;
            }
            auto value = registry.read(std::format(R"({}\{}\.Current)", kSchemes, relative), "");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            if (*value)
            {
                others[relative] = stringField(*value);
            }
        }
    }
    return others;
}

bool silent(const Json& value)
{
    return value.is_null() || (value.is_object() && json::stringOr(value, "data", "x").empty());
}

bool silentMarker(const Json& value)
{
    return value.is_string() && value.get_ref<const std::string&>() == kSilent;
}

} // namespace

std::span<const std::string_view> SoundsLogic::dependsOn() const noexcept
{
    return kDependsOn;
}

Result<Json> SoundsLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const
{
    Json desired = Json::object();
    if (!rice.sounds)
    {
        return desired;
    }
    const domain::SoundsConfig& config = *rice.sounds;
    if (config.scheme == domain::SoundScheme::None)
    {
        for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
        {
            desired[std::string(domain::riceName(static_cast<domain::SoundEvent>(i)))] = silence();
        }
        // Like Windows' "No Sounds": every event, not only the eighteen (v1.8 left the others playing).
        desired[std::string(kOthers)] = kSilent;
    }
    bool anyFile = false;
    for (const auto& assignment : config.events)
    {
        const std::string name(domain::riceName(assignment.event));
        if (assignment.file.empty())
        {
            desired[name] = silence();
            continue;
        }
        const std::string context = std::format("sound '{}'", assignment.file);
        auto bytes = assets.readPackageFile(assignment.file, limits::kSoundMaxBytes);
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()).withContext(context));
        }
        if (auto valid = domain::validateWaveFile(*bytes); !valid)
        {
            return std::unexpected(std::move(valid.error()).withContext(context));
        }
        auto stored = assets.fromPackage(assignment.file);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        const std::u8string storedPath = stored->u8string();
        desired[name] = Json{{"type", "expand"}, {"data", std::string(storedPath.begin(), storedPath.end())}};
        anyFile = true;
    }
    if (!desired.empty())
    {
        // Windows' names: ".None" is "No Sounds"; ".Current" is a scheme someone changed.
        desired["scheme"] = Json{{"type", "string"}, {"data", anyFile ? ".Current" : ".None"}};
    }
    return desired;
}

Result<void> SoundsLogic::internalize(Json& state, engine::IAssetSink& assets) const
{
    const auto keepCopy = [&](Json& field) {
        if (!field.is_object() || !field.contains("data") || !field["data"].is_string())
        {
            return;
        }
        const std::string path = field["data"].get<std::string>();
        const bool keep = json::boolOr(field, "keep", false);
        field.erase("keep");
        // Windows' own sounds never go away; anything with %VARS% we cannot resolve here.
        if (keep || path.empty() || startsWithIgnoreCase(path, "%SystemRoot%") ||
            startsWithIgnoreCase(path, "%windir%") || startsWithIgnoreCase(path, R"(C:\Windows\)") ||
            path.find('%') != std::string::npos)
        {
            return;
        }
        auto wide = utf8To16(path);
        if (!wide)
        {
            return;
        }
        auto stored = assets.fromSystem(std::filesystem::path(*wide), "wav");
        if (!stored)
        {
            // Missing or unreadable: keep the original path; a restore puts back what was there.
            log::warn("sounds: cannot keep a copy of {}: {}", path, stored.error().message());
            return;
        }
        const std::u8string storedPath = stored->u8string();
        field["copy"] = std::string(storedPath.begin(), storedPath.end());
    };
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        if (const auto field = state.find(domain::riceName(static_cast<domain::SoundEvent>(i))); field != state.end())
        {
            keepCopy(*field);
        }
    }
    if (const auto others = state.find(kOthers); others != state.end() && others->is_object())
    {
        for (auto& [event, field] : others->items())
        {
            keepCopy(field);
        }
    }
    return {};
}

bool SoundsLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    const auto plain = [](Json value) {
        if (value.is_object())
        {
            value.erase("copy");
            value.erase("keep");
        }
        return value;
    };
    if (field == kOthers)
    {
        // "silent" against what Windows shows: every one without a sound.
        const auto allSilent = [](const Json& map) {
            return map.is_object() && std::ranges::all_of(map.items(), [](const auto& item) { return silent(item.value()); });
        };
        if (silentMarker(a) || silentMarker(b))
        {
            return (silentMarker(a) || allSilent(a)) && (silentMarker(b) || allSilent(b));
        }
        // Two captures: the events both know sound the same (one an app added since is no change of ours).
        if (a.is_object() && b.is_object())
        {
            for (const auto& [event, value] : a.items())
            {
                if (const auto other = b.find(event); other != b.end() && plain(value) != plain(*other))
                {
                    return false;
                }
            }
            return true;
        }
    }
    return plain(a) == plain(b);
}

Result<engine::CaptureResult> SoundsSystem::capture()
{
    engine::CaptureResult result;
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        const auto event = static_cast<domain::SoundEvent>(i);
        auto value = m_registry.read(currentKey(event), "");
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        Json field = stringField(*value);
        if (field.is_object() && field.contains("data") && m_shell.isWindowsFile(field["data"].get<std::string>()))
        {
            field["keep"] = true; // Windows' own sound: never copied
        }
        result.state[std::string(domain::riceName(event))] = std::move(field);
    }
    auto scheme = m_registry.read(kSchemes, "");
    if (!scheme)
    {
        return std::unexpected(std::move(scheme.error()));
    }
    result.state["scheme"] = stringField(*scheme);
    auto others = otherEvents(m_registry);
    if (!others)
    {
        return std::unexpected(std::move(others.error()));
    }
    for (auto& [event, field] : others->items())
    {
        if (field.is_object() && field.contains("data") && m_shell.isWindowsFile(field["data"].get<std::string>()))
        {
            field["keep"] = true; // Windows' own sound: never copied
        }
    }
    result.state[std::string(kOthers)] = std::move(*others);
    return result;
}

Result<void> SoundsSystem::apply(const Json& desired)
{
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        const auto event = static_cast<domain::SoundEvent>(i);
        const std::string_view name = domain::riceName(event);
        if (const auto value = desired.find(name); value != desired.end())
        {
            if (auto written = writeStringField(m_registry, m_shell, currentKey(event), name, *value); !written)
            {
                return written;
            }
        }
    }
    if (const auto others = desired.find(kOthers); others != desired.end())
    {
        Json events = *others;
        if (silentMarker(*others))
        {
            auto now = otherEvents(m_registry);
            if (!now)
            {
                return std::unexpected(std::move(now.error()));
            }
            events = Json::object();
            for (const auto& [event, value] : now->items())
            {
                events[event] = silence();
            }
        }
        if (!events.is_object())
        {
            return fail(ErrorCode::InvalidArgument, "sound field 'others' is malformed");
        }
        for (const auto& [event, value] : events.items())
        {
            const auto valid = event.starts_with(R"(Apps\)") && event.find("..") == std::string::npos;
            if (!valid)
            {
                continue; // not a path under AppEvents\Schemes\Apps: never written
            }
            const std::string key = std::format(R"({}\{}\.Current)", kSchemes, event);
            if (auto written = writeStringField(m_registry, m_shell, key, event, value); !written)
            {
                return written;
            }
        }
    }
    if (const auto scheme = desired.find("scheme"); scheme != desired.end())
    {
        if (auto written = writeStringField(m_registry, m_shell, std::string(kSchemes), "scheme", *scheme); !written)
        {
            return written;
        }
    }
    return {}; // Windows reads them when it plays them: nothing to tell anyone
}

} // namespace rmw::modules
