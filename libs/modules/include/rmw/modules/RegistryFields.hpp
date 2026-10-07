#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/ports/Registry.hpp"

#include <optional>
#include <string_view>

// Fields backed by one DWORD registry value each. In a captured state, JSON null means "the value
// does not exist" (Windows uses its default), so a restore can delete it again instead of writing
// a guess — exact fidelity.
namespace rmw::modules
{

using json::Json;

struct DwordField
{
    std::string_view field; // name in the module state, e.g. "taskView"
    std::string_view key;   // HKCU subkey
    std::string_view value; // registry value name
};

// The registry DWORD as JSON: integer, null when absent, or a rawField() when it holds another type.
[[nodiscard]] Result<Json> readDwordField(ports::IRegistry& registry, const DwordField& field);

// Writes an integer (DWORD range), removes the value for null, writes a rawField() back as it was.
// Other JSON types are rejected.
[[nodiscard]] Result<void> writeDwordField(ports::IRegistry& registry, const DwordField& field,
                                           const Json& value);

// A registry value as JSON with its type ({"type": "dword"|"string"|"expand"|"binary", "data"}, plus
// "registryType" for the bytes of a type other than REG_BINARY), null when absent; and back.
[[nodiscard]] Json registryValueJson(const std::optional<ports::RegistryValue>& value);
[[nodiscard]] Result<ports::RegistryValue> registryValueFromJson(const Json& json);

// A value of a type the field does not expect (a string where Windows keeps a DWORD, a QWORD...):
// {"raw": registryValueJson()}. Read as absent, a restore deleted the user's value; kept as it is,
// a restore writes it back.
[[nodiscard]] Json rawField(const ports::RegistryValue& value);
[[nodiscard]] bool isRawField(const Json& field) noexcept;
[[nodiscard]] Result<void> writeRawField(ports::IRegistry& registry, std::string_view key, std::string_view name,
                                         const Json& field);

} // namespace rmw::modules
