#include "rmw/modules/RegistryFields.hpp"

#include <format>
#include <limits>

namespace rmw::modules
{

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
        // Someone wrote a string where Windows expects a DWORD; treat as absent but keep going.
        return Json(nullptr);
    }
    return Json((*value)->dword);
}

Result<void> writeDwordField(ports::IRegistry& registry, const DwordField& field, const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(field.key, field.value);
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
