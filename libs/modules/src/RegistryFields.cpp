#include "rmw/modules/RegistryFields.hpp"

#include <format>
#include <limits>

namespace rmw::modules
{
namespace
{

// What a registry value may hold (RegQueryValueEx's limit here), as hex.
constexpr std::size_t kMaxBinaryBytes = std::size_t{1} << 20;

std::string hex(const std::vector<std::byte>& bytes)
{
    static constexpr std::string_view kDigits = "0123456789abcdef";
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
    bytes.reserve(text.size() / 2);
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

} // namespace

Json registryValueJson(const std::optional<ports::RegistryValue>& value)
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
    case ports::RegistryValue::Type::Binary: {
        Json json{{"type", "binary"}, {"data", hex(value->bytes)}};
        if (value->registryType)
        {
            json["registryType"] = *value->registryType;
        }
        return json;
    }
    }
    return nullptr;
}

Result<ports::RegistryValue> registryValueFromJson(const Json& json)
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
        ports::RegistryValue value{ports::RegistryValue::Type::Binary, 0, {}, std::move(*bytes), {}};
        if (const Json kind = json::memberOr(json, "registryType", Json()); !kind.is_null())
        {
            if (!kind.is_number_unsigned() || kind.get<std::uint64_t>() > 0xFFFF'FFFFU)
            {
                return fail(ErrorCode::InvalidData, "bad registry type");
            }
            value.registryType = kind.get<std::uint32_t>();
        }
        return value;
    }
    return fail(ErrorCode::InvalidArgument, "a registry value is { type = dword|string|expand|binary, data }");
}

Json rawField(const ports::RegistryValue& value)
{
    return Json{{"raw", registryValueJson(value)}};
}

bool isRawField(const Json& field) noexcept
{
    return field.is_object() && field.size() == 1 && field.contains("raw");
}

Result<void> writeRawField(ports::IRegistry& registry, std::string_view key, std::string_view name,
                           const Json& field)
{
    auto value = registryValueFromJson(json::memberOr(field, "raw", Json()));
    if (!value)
    {
        return std::unexpected(std::move(value.error()).withContext(std::format("registry value '{}'", name)));
    }
    return registry.write(key, name, *value);
}

Result<Json> readDwordField(ports::IRegistry& registry, const DwordField& field)
{
    auto value = registry.read(field.key, field.value);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!*value)
    {
        return Json(nullptr);
    }
    if ((*value)->type != ports::RegistryValue::Type::Dword)
    {
        // Someone wrote a string where Windows expects a DWORD: kept as it is for a restore.
        return rawField(**value);
    }
    return Json((*value)->dword);
}

Result<void> writeDwordField(ports::IRegistry& registry, const DwordField& field, const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(field.key, field.value);
    }
    if (isRawField(value))
    {
        return writeRawField(registry, field.key, field.value, value);
    }
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0 ||
        value.get<std::int64_t>() > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorCode::InvalidArgument, std::format("field '{}' needs a DWORD or null", field.field));
    }
    return registry.write(
        field.key, field.value,
        ports::RegistryValue::fromDword(static_cast<std::uint32_t>(value.get<std::int64_t>())));
}

} // namespace rmw::modules
