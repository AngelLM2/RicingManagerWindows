#include "rmw/modules/Tweaks.hpp"

#include "rmw/domain/Tweaks.hpp"

#include <format>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr char kSeparator = '|';
constexpr std::size_t kMaxBinaryBytes = 4096;

std::string hex(const std::vector<std::byte>& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::byte b : bytes)
    {
        text += kDigits[static_cast<unsigned>(b) >> 4U];
        text += kDigits[static_cast<unsigned>(b) & 0xFU];
    }
    return text;
}

Result<std::vector<std::byte>> unhex(std::string_view text)
{
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        return -1;
    };
    if (text.size() % 2 != 0 || text.size() / 2 > kMaxBinaryBytes)
    {
        return fail(ErrorCode::InvalidData, "bad binary value");
    }
    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
    {
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0)
        {
            return fail(ErrorCode::InvalidData, "bad binary value");
        }
        bytes.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return bytes;
}

Json toJson(const std::optional<ports::RegistryValue>& value)
{
    if (!value)
    {
        return nullptr;
    }
    switch (value->type)
    {
    case ports::RegistryValue::Type::Dword:
        return Json{{"type", "dword"}, {"data", value->dword}};
    case ports::RegistryValue::Type::String:
        return Json{{"type", "string"}, {"data", value->text}};
    case ports::RegistryValue::Type::ExpandString:
        return Json{{"type", "expand"}, {"data", value->text}};
    case ports::RegistryValue::Type::Binary:
        return Json{{"type", "binary"}, {"data", hex(value->bytes)}};
    }
    return nullptr;
}

Result<ports::RegistryValue> fromJson(const Json& json)
{
    const std::string type = json::stringOr(json, "type", "");
    const Json data = json::memberOr(json, "data", Json());
    if (type == "dword" && data.is_number_unsigned() && data.get<std::uint64_t>() <= 0xFFFF'FFFFU)
    {
        return ports::RegistryValue::fromDword(data.get<std::uint32_t>());
    }
    if (type == "dword" && data.is_number_integer() && data.get<std::int64_t>() >= 0 &&
        data.get<std::int64_t>() <= 0xFFFF'FFFF)
    {
        return ports::RegistryValue::fromDword(static_cast<std::uint32_t>(data.get<std::int64_t>()));
    }
    if ((type == "string" || type == "expand") && data.is_string())
    {
        return type == "string" ? ports::RegistryValue::fromString(data.get<std::string>())
                                : ports::RegistryValue::fromExpandString(data.get<std::string>());
    }
    if (type == "binary" && data.is_string())
    {
        auto bytes = unhex(data.get<std::string>());
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()));
        }
        return ports::RegistryValue{ports::RegistryValue::Type::Binary, 0, {}, std::move(*bytes)};
    }
    return fail(ErrorCode::InvalidArgument, "a tweak value is { type = dword|string|expand|binary, data }");
}

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
        result.state[field] = toJson(*value);
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
            auto converted = fromJson(value);
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
