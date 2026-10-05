#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rmw::domain
{

struct ValueMember;

// Free-form configuration tree (plugin and widget settings). Mirrors what TOML and JSON can
// express so it crosses the IPC boundary unchanged; validated later against the plugin's schema.
struct Value
{
    using Array = std::vector<Value>;
    using Table = std::vector<ValueMember>; // unique keys; from TOML they come sorted

    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Table> data;

    bool isNull() const noexcept { return std::holds_alternative<std::monostate>(data); }
    const Table* table() const noexcept { return std::get_if<Table>(&data); }
    const Value* find(std::string_view key) const noexcept;

    friend bool operator==(const Value&, const Value&) = default;
};

struct ValueMember
{
    std::string key;
    Value value;

    friend bool operator==(const ValueMember&, const ValueMember&) = default;
};

inline const Value* Value::find(std::string_view key) const noexcept
{
    if (const Table* members = table())
    {
        for (const auto& member : *members)
        {
            if (member.key == key)
            {
                return &member.value;
            }
        }
    }
    return nullptr;
}

} // namespace rmw::domain
