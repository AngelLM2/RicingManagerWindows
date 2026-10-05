#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/base/Limits.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// JSON helpers: parsing with limits, dumping that never throws, and typed reads that report
// "expected a string at config.monitors[2].path" instead of throwing nlohmann::type_error.
namespace rmw::json
{

using Json = nlohmann::json;

struct ParseLimits
{
    std::size_t maxBytes = limits::kIpcMaxMessageBytes;
    std::size_t maxDepth = limits::kJsonMaxDepth;
    std::size_t maxElements = limits::kJsonMaxElements;
};

// Parses untrusted text. Size, nesting depth and element count are checked on the raw text
// before nlohmann sees it.
[[nodiscard]] Result<Json> parse(std::string_view text, const ParseLimits& limits = {});

// Compact text. Invalid UTF-8 in strings is replaced instead of throwing.
[[nodiscard]] std::string dump(const Json& value);

// Read-only view of a JSON object with a path for error messages. Every getter fails with
// InvalidData (never throws) when the key is missing or has the wrong type.
class Reader
{
public:
    Reader(const Json& value, std::string path);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

    [[nodiscard]] Result<std::string> string(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> integer(std::string_view key) const;
    [[nodiscard]] Result<double> number(std::string_view key) const;
    [[nodiscard]] Result<bool> boolean(std::string_view key) const;
    [[nodiscard]] Result<Reader> object(std::string_view key) const;
    [[nodiscard]] Result<const Json*> array(std::string_view key) const;

    // Optional variants: nullopt when absent, error when present with the wrong type.
    [[nodiscard]] Result<std::optional<std::string>> optionalString(std::string_view key) const;
    [[nodiscard]] Result<std::optional<std::int64_t>> optionalInteger(std::string_view key) const;
    [[nodiscard]] Result<std::optional<bool>> optionalBoolean(std::string_view key) const;

    [[nodiscard]] const Json& raw() const noexcept { return *m_value; }

private:
    [[nodiscard]] Result<const Json*> member(std::string_view key) const;
    [[nodiscard]] Error typeError(std::string_view key, std::string_view expected, const Json& found) const;

    const Json* m_value;
    std::string m_path;
};

// object[key] when present with the right type, otherwise `fallback`. Never throws (unlike
// nlohmann's value(), which throws when the key exists with another type).
[[nodiscard]] bool boolOr(const Json& object, std::string_view key, bool fallback);
[[nodiscard]] std::int64_t intOr(const Json& object, std::string_view key, std::int64_t fallback);
[[nodiscard]] std::string stringOr(const Json& object, std::string_view key, std::string_view fallback);
[[nodiscard]] Json memberOr(const Json& object, std::string_view key, Json fallback);

// "object", "string"... for messages.
[[nodiscard]] std::string_view typeName(const Json& value) noexcept;

} // namespace rmw::json
