#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/ports/Registry.hpp"

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

// The registry DWORD as JSON: integer, or null when absent.
[[nodiscard]] Result<Json> readDwordField(ports::IRegistry& registry, const DwordField& field);

// Writes an integer (DWORD range) or removes the value for null. Other JSON types are rejected.
[[nodiscard]] Result<void> writeDwordField(ports::IRegistry& registry, const DwordField& field,
                                           const Json& value);

} // namespace rmw::modules
