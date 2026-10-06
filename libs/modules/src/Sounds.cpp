#include "rmw/modules/Sounds.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/SoundFiles.hpp"

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
    if (!value || (value->type != ports::RegistryValue::Type::String &&
                   value->type != ports::RegistryValue::Type::ExpandString))
    {
        return nullptr;
    }
    return Json{{"type", value->type == ports::RegistryValue::Type::String ? "string" : "expand"},
                {"data", value->text}};
}

Result<void> writeStringField(ports::IRegistry& registry, const std::string& key, std::string_view field,
                              const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(key, "");
    }
    const auto type = value.find("type");
    const auto data = value.find("data");
    if (!value.is_object() || type == value.end() || data == value.end() || !type->is_string() ||
        !data->is_string())
    {
        return fail(ErrorCode::InvalidArgument, std::format("sound field '{}' is malformed", field));
    }
    const auto& text = data->get_ref<const std::string&>();
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
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        const std::string name(domain::riceName(static_cast<domain::SoundEvent>(i)));
        const auto field = state.find(name);
        if (field == state.end() || !field->is_object() || !field->contains("data") ||
            !(*field)["data"].is_string())
        {
            continue;
        }
        const std::string path = (*field)["data"].get<std::string>();
        const bool keep = json::boolOr(*field, "keep", false);
        field->erase("keep");
        // Windows' own sounds never go away; anything with %VARS% we cannot resolve here.
        if (keep || path.empty() || startsWithIgnoreCase(path, "%SystemRoot%") ||
            startsWithIgnoreCase(path, "%windir%") || path.find('%') != std::string::npos)
        {
            continue;
        }
        auto wide = utf8To16(path);
        if (!wide)
        {
            continue;
        }
        auto stored = assets.fromSystem(std::filesystem::path(*wide), "wav");
        if (!stored)
        {
            // Missing or unreadable: keep the original path; a restore puts back what was there.
            log::warn("sounds: cannot keep a copy of {}: {}", path, stored.error().message());
            continue;
        }
        const std::u8string storedPath = stored->u8string();
        (*field)["data"] = std::string(storedPath.begin(), storedPath.end());
    }
    return {};
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
        if (field.is_object() && m_shell.isWindowsFile(field["data"].get<std::string>()))
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
            if (auto written = writeStringField(m_registry, currentKey(event), name, *value); !written)
            {
                return written;
            }
        }
    }
    if (const auto scheme = desired.find("scheme"); scheme != desired.end())
    {
        if (auto written = writeStringField(m_registry, std::string(kSchemes), "scheme", *scheme); !written)
        {
            return written;
        }
    }
    return {}; // Windows reads them when it plays them: nothing to tell anyone
}

} // namespace rmw::modules
