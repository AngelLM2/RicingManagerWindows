#include "rmw/json/Json.hpp"

#include <format>
#include <limits>

namespace rmw::json
{
namespace
{

// Depth and element count straight from the text, so hostile input never reaches nlohmann's
// recursive serializer (or our own walkers) deeper than allowed.
Result<void> checkShape(std::string_view text, const ParseLimits& limits)
{
    std::size_t depth = 0;
    std::size_t elements = 1;
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (c == '\\')
            {
                ++i;
            }
            else if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        switch (c)
        {
        case '"':
            inString = true;
            break;
        case '[':
        case '{':
            if (++depth > limits.maxDepth)
            {
                return fail(ErrorCode::LimitExceeded,
                            std::format("JSON nesting is too deep (limit {})", limits.maxDepth));
            }
            ++elements;
            break;
        case ']':
        case '}':
            depth -= depth > 0 ? 1 : 0;
            break;
        case ',':
            ++elements;
            break;
        default:
            break;
        }
        if (elements > limits.maxElements)
        {
            return fail(ErrorCode::LimitExceeded,
                        std::format("JSON has too many elements (limit {})", limits.maxElements));
        }
    }
    return {};
}

std::string childPath(const std::string& parent, std::string_view key)
{
    return parent.empty() ? std::string(key) : std::format("{}.{}", parent, key);
}

} // namespace

std::string_view typeName(const Json& value) noexcept
{
    switch (value.type())
    {
    case Json::value_t::null:
        return "null";
    case Json::value_t::object:
        return "object";
    case Json::value_t::array:
        return "array";
    case Json::value_t::string:
        return "string";
    case Json::value_t::boolean:
        return "boolean";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "integer";
    case Json::value_t::number_float:
        return "number";
    case Json::value_t::binary:
        return "binary";
    case Json::value_t::discarded:
        break;
    }
    return "invalid";
}

Result<Json> parse(std::string_view text, const ParseLimits& limits)
{
    if (text.size() > limits.maxBytes)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("JSON is too large ({} bytes, limit {})", text.size(), limits.maxBytes));
    }
    if (auto shape = checkShape(text, limits); !shape)
    {
        return std::unexpected(std::move(shape.error()));
    }
    try
    {
        return Json::parse(text.begin(), text.end());
    }
    catch (const Json::exception& e)
    {
        return fail(ErrorCode::InvalidData, std::format("malformed JSON: {}", e.what()));
    }
}

bool boolOr(const Json& object, std::string_view key, bool fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::int64_t intOr(const Json& object, std::string_view key, std::int64_t fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer())
    {
        return fallback;
    }
    if (it->is_number_unsigned() &&
        it->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return fallback;
    }
    return it->get<std::int64_t>();
}

std::string stringOr(const Json& object, std::string_view key, std::string_view fallback)
{
    if (object.is_object())
    {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string())
        {
            return it->get<std::string>();
        }
    }
    return std::string(fallback);
}

Json memberOr(const Json& object, std::string_view key, Json fallback)
{
    if (object.is_object())
    {
        const auto it = object.find(key);
        if (it != object.end())
        {
            return *it;
        }
    }
    return fallback;
}

std::string dumpPretty(const Json& value)
{
    return value.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::string dump(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Reader::Reader(const Json& value, std::string path)
    : m_value(&value)
    , m_path(std::move(path))
{
}

bool Reader::has(std::string_view key) const
{
    return m_value->is_object() && m_value->contains(key);
}

Result<const Json*> Reader::member(std::string_view key) const
{
    if (!m_value->is_object())
    {
        return fail(ErrorCode::InvalidData,
                    std::format("expected an object at '{}', found {}", m_path.empty() ? "<root>" : m_path,
                                typeName(*m_value)));
    }
    const auto it = m_value->find(key);
    if (it == m_value->end())
    {
        return fail(ErrorCode::InvalidData, std::format("missing '{}'", childPath(m_path, key)));
    }
    return &*it;
}

Error Reader::typeError(std::string_view key, std::string_view expected, const Json& found) const
{
    return {ErrorCode::InvalidData,
            std::format("expected {} at '{}', found {}", expected, childPath(m_path, key), typeName(found))};
}

Result<std::string> Reader::string(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_string())
    {
        return std::unexpected(typeError(key, "a string", **value));
    }
    return (*value)->get_ref<const std::string&>();
}

Result<std::int64_t> Reader::integer(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    const Json& json = **value;
    if (json.is_number_unsigned())
    {
        const auto number = json.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return fail(ErrorCode::InvalidData,
                        std::format("integer out of range at '{}'", childPath(m_path, key)));
        }
        return static_cast<std::int64_t>(number);
    }
    if (!json.is_number_integer())
    {
        return std::unexpected(typeError(key, "an integer", json));
    }
    return json.get<std::int64_t>();
}

Result<double> Reader::number(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_number())
    {
        return std::unexpected(typeError(key, "a number", **value));
    }
    return (*value)->get<double>();
}

Result<bool> Reader::boolean(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_boolean())
    {
        return std::unexpected(typeError(key, "true or false", **value));
    }
    return (*value)->get<bool>();
}

Result<Reader> Reader::object(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_object())
    {
        return std::unexpected(typeError(key, "an object", **value));
    }
    return Reader(**value, childPath(m_path, key));
}

Result<const Json*> Reader::array(std::string_view key) const
{
    auto value = member(key);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_array())
    {
        return std::unexpected(typeError(key, "an array", **value));
    }
    return *value;
}

Result<std::optional<std::string>> Reader::optionalString(std::string_view key) const
{
    if (!has(key))
    {
        return std::optional<std::string>{};
    }
    return string(key).transform([](std::string s) { return std::optional<std::string>(std::move(s)); });
}

Result<std::optional<std::int64_t>> Reader::optionalInteger(std::string_view key) const
{
    if (!has(key))
    {
        return std::optional<std::int64_t>{};
    }
    return integer(key).transform([](std::int64_t v) { return std::optional<std::int64_t>(v); });
}

Result<std::optional<bool>> Reader::optionalBoolean(std::string_view key) const
{
    if (!has(key))
    {
        return std::optional<bool>{};
    }
    return boolean(key).transform([](bool v) { return std::optional<bool>(v); });
}

} // namespace rmw::json
