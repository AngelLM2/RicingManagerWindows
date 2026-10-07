#include "rmw/modules/Tweaks.hpp"

#include "rmw/domain/Tweaks.hpp"
#include "rmw/modules/RegistryFields.hpp"

#include <format>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr char kSeparator = '|';

} // namespace

std::string tweakField(std::string_view key, std::string_view value)
{
    return std::format("{}{}{}", key, kSeparator, value);
}

Result<TweakTarget> parseTweakField(std::string_view field)
{
    const auto separator = field.rfind(kSeparator);
    if (separator == std::string_view::npos)
    {
        return fail(ErrorCode::InvalidArgument, "a tweak field is \"<key>|<value name>\"");
    }
    // Checked again on this side: a field reaches the registry only through the allow-list.
    auto key = domain::validateTweakKey(field.substr(0, separator));
    if (!key)
    {
        return std::unexpected(std::move(key.error()));
    }
    const std::string_view name = field.substr(separator + 1);
    if (auto valid = domain::validateTweakValueName(*key, name); !valid)
    {
        return std::unexpected(std::move(valid.error()));
    }
    return TweakTarget{std::move(*key), std::string(name)};
}

Result<Json> TweaksLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& /*assets*/) const
{
    Json desired = Json::object();
    for (const auto& tweak : rice.tweaks)
    {
        auto target = parseTweakField(tweakField(tweak.key, tweak.value));
        if (!target)
        {
            return std::unexpected(std::move(target.error()));
        }
        Json value;
        if (const auto* number = std::get_if<std::uint64_t>(&tweak.data);
            number != nullptr && tweak.type == domain::RegistryType::Dword)
        {
            if (*number > 0xFFFF'FFFFU)
            {
                return fail(ErrorCode::InvalidData, "a DWORD tweak is at most 0xFFFFFFFF");
            }
            value = Json{{"type", "dword"}, {"data", *number}};
        }
        else if (const auto* text = std::get_if<std::string>(&tweak.data);
                 text != nullptr && tweak.type == domain::RegistryType::String)
        {
            if (auto valid = domain::validateTweakString(*text); !valid)
            {
                return std::unexpected(std::move(valid.error()));
            }
            value = Json{{"type", "string"}, {"data", *text}};
        }
        else
        {
            return fail(ErrorCode::InvalidData, "a tweak's data does not match its type");
        }
        desired[tweakField(target->key, target->value)] = std::move(value);
    }
    return desired;
}

Result<engine::CaptureResult> TweaksSystem::capture()
{
    return engine::CaptureResult{};
}

Result<engine::CaptureResult> TweaksSystem::captureFields(std::span<const std::string> fields)
{
    engine::CaptureResult result;
    for (const auto& field : fields)
    {
        auto target = parseTweakField(field);
        if (!target)
        {
            continue; // not ours to read
        }
        auto value = m_registry.read(target->key, target->value);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[field] = registryValueJson(*value);
    }
    return result;
}

Result<void> TweaksSystem::apply(const engine::Json& desired)
{
    bool changed = false;
    for (const auto& [field, value] : desired.items())
    {
        auto target = parseTweakField(field);
        if (!target)
        {
            return std::unexpected(std::move(target.error()));
        }
        if (value.is_null())
        {
            if (auto removed = m_registry.remove(target->key, target->value); !removed)
            {
                return removed;
            }
        }
        else
        {
            auto converted = registryValueFromJson(value);
            if (!converted)
            {
                return std::unexpected(std::move(converted.error()));
            }
            if (auto written = m_registry.write(target->key, target->value, *converted); !written)
            {
                return written;
            }
        }
        changed = true;
    }
    if (changed)
    {
        // Explorer and the shell re-read many of these on this broadcast; the rest need a restart
        // of Explorer, which the plugin says (restart_explorer) and we only tell the user.
        return m_shell.broadcastSettingChange("TraySettings");
    }
    return {};
}

} // namespace rmw::modules
