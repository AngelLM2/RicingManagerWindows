#pragma once

#include "EnumNames.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/Value.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Our loader on top of toml++ (internal to rmw::domain): the only place that touches toml++
// types, so rice.toml and plugin.toml share limits, error wording and unknown-key warnings.
namespace rmw::domain::detail
{

// Parses a manifest. Enforces the size limit before parsing and the depth limit after; syntax
// errors become a located diagnostic. toml++ exceptions never leave this function.
std::optional<toml::table> parseToml(std::string_view text, Diagnostics& diagnostics);

std::string childPath(std::string_view parent, std::string_view key);
std::string indexPath(std::string_view parent, std::size_t index);

std::string_view typeName(const toml::node& node) noexcept;

// Rejects control characters (except tab, and newline when `multiline`) and Unicode bidi
// overrides, which can make a name look like something else in a confirmation dialog.
bool isDisplaySafeText(std::string_view text, bool multiline) noexcept;

// Converts a TOML subtree to a Value. `budget` counts the elements still allowed.
std::optional<Value> toValue(const toml::node& node, const std::string& path, Diagnostics& diagnostics,
                             std::size_t& budget);

void errorAt(Diagnostics& diagnostics, const toml::node& node, std::string path, std::string message);
void warningAt(Diagnostics& diagnostics, const toml::node& node, std::string path, std::string message);

// Typed access to one TOML table. Every key asked for is remembered, so reportUnknownKeys()
// can warn about the rest ("unknown key — typo?") instead of silently ignoring them.
class TableReader
{
public:
    TableReader(const toml::table& table, std::string path, Diagnostics& diagnostics);
    ~TableReader() = default;
    TableReader(const TableReader&) = delete;
    TableReader& operator=(const TableReader&) = delete;
    TableReader(TableReader&&) = delete;
    TableReader& operator=(TableReader&&) = delete;

    const std::string& path() const noexcept { return m_path; }
    std::string pathOf(std::string_view key) const { return childPath(m_path, key); }
    const toml::table& raw() const noexcept { return m_table; }
    Diagnostics& diagnostics() noexcept { return m_diagnostics; }

    const toml::node* get(std::string_view key); // marks the key as known

    std::optional<std::string> string(std::string_view key,
                                      std::size_t maxBytes = limits::kShortTextMaxBytes);
    std::optional<bool> boolean(std::string_view key);
    std::optional<std::int64_t> integer(std::string_view key, std::int64_t min, std::int64_t max);
    std::optional<double> number(std::string_view key, double min, double max); // integer or float
    const toml::table* table(std::string_view key);
    const toml::array* array(std::string_view key, std::size_t maxElements);

    template <class E>
    std::optional<E> enumeration(std::string_view key, std::span<const EnumName<E>> names)
    {
        const auto text = string(key);
        if (!text)
        {
            return std::nullopt;
        }
        if (const auto value = enumFromName(names, *text))
        {
            return value;
        }
        error(key, std::format("unknown value '{}' (expected {})", *text, enumChoices(names)));
        return std::nullopt;
    }

    // Reads a string and runs it through a Result-returning parser (Color::parse, SemVer::parse...).
    template <class Parser>
    auto parsed(std::string_view key, Parser parser)
        -> std::optional<typename decltype(parser(std::string_view{}))::value_type>
    {
        const auto text = string(key);
        if (!text)
        {
            return std::nullopt;
        }
        auto result = parser(std::string_view(*text));
        if (!result)
        {
            error(key, result.error().message());
            return std::nullopt;
        }
        return std::move(*result);
    }

    void error(std::string_view key, std::string message);   // located at the key's value
    void warning(std::string_view key, std::string message); // located at the key's value
    void missing(std::string_view key);                      // located at the table

    // Warns once about every key that was never asked for. Call after reading the table.
    void reportUnknownKeys();

private:
    std::pair<std::uint32_t, std::uint32_t> locate(std::string_view key) const;

    const toml::table& m_table;
    std::string m_path;
    Diagnostics& m_diagnostics;
    std::vector<std::string> m_known;
};

} // namespace rmw::domain::detail
