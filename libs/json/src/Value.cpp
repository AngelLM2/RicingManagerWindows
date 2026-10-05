#include "rmw/json/Value.hpp"

#include <type_traits>

namespace rmw::json
{

Json fromValue(const domain::Value& value)
{
    return std::visit(
        [](const auto& data) -> Json {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return nullptr;
            }
            else if constexpr (std::is_same_v<T, domain::Value::Array>)
            {
                Json array = Json::array();
                for (const auto& item : data)
                {
                    array.push_back(fromValue(item));
                }
                return array;
            }
            else if constexpr (std::is_same_v<T, domain::Value::Table>)
            {
                Json object = Json::object();
                for (const auto& member : data)
                {
                    object[member.key] = fromValue(member.value);
                }
                return object;
            }
            else
            {
                return data;
            }
        },
        value.data);
}

Result<domain::Value> toValue(const Json& json, std::size_t depth)
{
    if (depth > kValueMaxDepth)
    {
        return fail(ErrorCode::LimitExceeded, "values are nested too deeply");
    }
    domain::Value value;
    if (json.is_boolean())
    {
        value.data = json.get<bool>();
    }
    else if (json.is_number_integer())
    {
        value.data = json.get<std::int64_t>();
    }
    else if (json.is_number())
    {
        value.data = json.get<double>();
    }
    else if (json.is_string())
    {
        value.data = json.get<std::string>();
    }
    else if (json.is_array())
    {
        domain::Value::Array array;
        for (const auto& item : json)
        {
            auto converted = toValue(item, depth + 1);
            if (!converted)
            {
                return converted;
            }
            array.push_back(std::move(*converted));
        }
        value.data = std::move(array);
    }
    else if (json.is_object())
    {
        domain::Value::Table table;
        for (const auto& [key, item] : json.items())
        {
            auto converted = toValue(item, depth + 1);
            if (!converted)
            {
                return converted;
            }
            table.push_back({key, std::move(*converted)});
        }
        value.data = std::move(table);
    }
    return value;
}

} // namespace rmw::json
