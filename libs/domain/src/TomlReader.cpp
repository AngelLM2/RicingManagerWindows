#include "TomlReader.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace rmw::domain::detail
{
namespace
{

std::uint32_t lineOf(const toml::node& node) noexcept
{
    return node.source().begin.line;
}

std::uint32_t columnOf(const toml::node& node) noexcept
{
    return node.source().begin.column;
}

constexpr std::string_view kBasicMultiline = R"(""")";
constexpr std::string_view kLiteralMultiline = "'''";

// Index just past the string starting at text[start] ('"', '\'', '"""' or '\'\'\''); text.size()
// when it is not terminated.
std::size_t skipString(std::string_view text, std::size_t start) noexcept
{
    const char quote = text[start];
    const bool basic = quote == '"';
    const bool multiline = text.substr(start, 3) == (basic ? kBasicMultiline : kLiteralMultiline);
    std::size_t i = start + (multiline ? 3 : 1);
    while (i < text.size())
    {
        const char c = text[i];
        if (basic && c == '\\')
        {
            i += 2;
            continue;
        }
        if (!multiline && c == '\n')
        {
            return i; // unterminated single-line string; let toml++ report it
        }
        if (c == quote)
        {
            if (!multiline)
            {
                return i + 1;
            }
            if (text.substr(i, 3) == (basic ? kBasicMultiline : kLiteralMultiline))
            {
                i += 3;
                // Up to two quotes right before the delimiter belong to the content.
                for (int extra = 0; extra < 2 && i < text.size() && text[i] == quote; ++extra)
                {
                    ++i;
                }
                return i;
            }
        }
        ++i;
    }
    return text.size();
}

// Nesting depth the document would produce (root = 0), computed from the tokens alone. toml++
// walks nested tables recursively while finishing a parse and while destroying the tree, so a
// 64 KB file of "a.a.a.a… = 1" would exhaust the stack before the tree could be checked. Exact
// for valid TOML (an empty array counts one level deeper); invalid input is rejected by toml++
// at its first error, and only the valid prefix — which this scan reads the same way — is built.
std::size_t scanNestingDepth(std::string_view text) noexcept
{
    constexpr std::size_t kTooDeep = limits::kTomlMaxDepth + 1;
    struct Container
    {
        bool inlineTable;
        std::size_t depth;
    };
    std::array<Container, kTooDeep + 1> stack{};
    std::size_t stackSize = 0;

    std::size_t base = 0;       // depth of the table opened by the last [header]
    std::size_t dots = 0;       // dots in the key being read
    std::size_t valueDepth = 0; // depth of the value being read
    std::size_t maxDepth = 0;
    bool keyContext = true; // reading a key (top level or inside an inline table)
    bool lineStart = true;  // top level, nothing but whitespace yet on this line

    const auto note = [&](std::size_t depth) { maxDepth = depth > maxDepth ? depth : maxDepth; };
    const auto afterValue = [&] {
        if (stackSize > 0 && !stack[stackSize - 1].inlineTable)
        {
            valueDepth = stack[stackSize - 1].depth + 1;
        }
        keyContext = false;
    };

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '#')
        {
            while (i + 1 < text.size() && text[i + 1] != '\n')
            {
                ++i;
            }
            continue;
        }
        if (c == '"' || c == '\'')
        {
            i = skipString(text, i) - 1;
            lineStart = false;
            continue;
        }
        if (c == '\n')
        {
            if (stackSize == 0)
            {
                keyContext = true;
                lineStart = true;
                dots = 0;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r')
        {
            continue;
        }

        if (keyContext)
        {
            if (stackSize == 0 && lineStart && c == '[')
            {
                // [table] or [[array.of.tables]]: count the dots up to the closing bracket.
                const bool arrayOfTables = i + 1 < text.size() && text[i + 1] == '[';
                std::size_t headerDots = 0;
                for (++i; i < text.size() && text[i] != ']' && text[i] != '\n'; ++i)
                {
                    if (text[i] == '"' || text[i] == '\'')
                    {
                        i = skipString(text, i) - 1;
                    }
                    else if (text[i] == '.')
                    {
                        ++headerDots;
                    }
                }
                base = headerDots + 1 + (arrayOfTables ? 1 : 0);
                note(base);
                if (arrayOfTables && i + 1 < text.size() && text[i + 1] == ']')
                {
                    ++i;
                }
                lineStart = false;
                dots = 0;
                continue;
            }
            lineStart = false;
            if (c == '.')
            {
                ++dots;
            }
            else if (c == '=')
            {
                valueDepth = (stackSize == 0 ? base : stack[stackSize - 1].depth) + dots + 1;
                note(valueDepth);
                keyContext = false;
                dots = 0;
            }
            else if (c == '}' && stackSize > 0 && stack[stackSize - 1].inlineTable)
            {
                --stackSize; // empty inline table, or trailing comma
                afterValue();
            }
            continue;
        }

        switch (c)
        {
        case '[':
        case '{':
            if (stackSize == kTooDeep)
            {
                return kTooDeep;
            }
            stack[stackSize++] = {c == '{', valueDepth};
            if (c == '[')
            {
                valueDepth += 1; // elements
                note(valueDepth);
            }
            else
            {
                keyContext = true;
                dots = 0;
            }
            break;
        case ']':
        case '}':
            if (stackSize > 0 && stack[stackSize - 1].inlineTable == (c == '}'))
            {
                --stackSize;
                afterValue();
            }
            break;
        case ',':
            if (stackSize > 0 && stack[stackSize - 1].inlineTable)
            {
                keyContext = true;
                dots = 0;
            }
            break;
        default:
            break;
        }
    }
    return maxDepth;
}

// Depth of nested tables/arrays below `node` (the document root is depth 0).
bool withinDepth(const toml::node& node, std::size_t depth) noexcept
{
    if (depth > limits::kTomlMaxDepth)
    {
        return false;
    }
    if (const auto* table = node.as_table())
    {
        for (const auto& [key, child] : *table)
        {
            if (!withinDepth(child, depth + 1))
            {
                return false;
            }
        }
    }
    else if (const auto* array = node.as_array())
    {
        for (const auto& child : *array)
        {
            if (!withinDepth(child, depth + 1))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::optional<toml::table> parseToml(std::string_view text, Diagnostics& diagnostics)
{
    if (text.size() > limits::kManifestMaxBytes)
    {
        diagnostics.error({}, std::format("file is too large ({} bytes, limit {})", text.size(),
                                          limits::kManifestMaxBytes));
        return std::nullopt;
    }
    if (scanNestingDepth(text) > limits::kTomlMaxDepth)
    {
        diagnostics.error({}, std::format("nesting is too deep (limit {})", limits::kTomlMaxDepth));
        return std::nullopt;
    }

    toml::table root;
    try
    {
        root = toml::parse(text);
    }
    catch (const toml::parse_error& e)
    {
        diagnostics.error({}, std::string(e.description()), e.source().begin.line, e.source().begin.column);
        return std::nullopt;
    }

    if (!withinDepth(root, 0))
    {
        diagnostics.error({}, std::format("nesting is too deep (limit {})", limits::kTomlMaxDepth));
        return std::nullopt;
    }
    return root;
}

std::string childPath(std::string_view parent, std::string_view key)
{
    if (parent.empty())
    {
        return std::string(key);
    }
    // Quote keys that would not read back as a bare TOML key (plugin ids contain dots).
    const bool bare = std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
    if (bare && !key.empty())
    {
        return std::format("{}.{}", parent, key);
    }
    return std::format("{}.\"{}\"", parent, key);
}

std::string indexPath(std::string_view parent, std::size_t index)
{
    return std::format("{}[{}]", parent, index);
}

std::string_view typeName(const toml::node& node) noexcept
{
    switch (node.type())
    {
    case toml::node_type::table:
        return "table";
    case toml::node_type::array:
        return "array";
    case toml::node_type::string:
        return "string";
    case toml::node_type::integer:
        return "integer";
    case toml::node_type::floating_point:
        return "float";
    case toml::node_type::boolean:
        return "boolean";
    case toml::node_type::date:
    case toml::node_type::time:
    case toml::node_type::date_time:
        return "date/time";
    case toml::node_type::none:
        break;
    }
    return "nothing";
}

bool isDisplaySafeText(std::string_view text, bool multiline) noexcept
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x20 || byte == 0x7F)
        {
            if (byte == '\t' || (multiline && byte == '\n'))
            {
                continue;
            }
            return false;
        }
        // U+202A..U+202E are E2 80 AA..AE; U+2066..U+2069 are E2 81 A6..A9.
        if (byte == 0xE2 && i + 2 < text.size())
        {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 == 0x80 && b2 >= 0xAA && b2 <= 0xAE) || (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9))
            {
                return false;
            }
        }
    }
    return true;
}

std::optional<Value> toValue(const toml::node& node, const std::string& path, Diagnostics& diagnostics,
                             std::size_t& budget)
{
    if (budget == 0)
    {
        errorAt(diagnostics, node, path,
                std::format("too many settings (limit {})", limits::kConfigValueMaxElements));
        return std::nullopt;
    }
    --budget;

    Value value;
    switch (node.type())
    {
    case toml::node_type::boolean:
        value.data = node.as_boolean()->get();
        return value;
    case toml::node_type::integer:
        value.data = node.as_integer()->get();
        return value;
    case toml::node_type::floating_point: {
        const double number = node.as_floating_point()->get();
        if (!std::isfinite(number))
        {
            errorAt(diagnostics, node, path, "nan and inf are not allowed");
            return std::nullopt;
        }
        value.data = number;
        return value;
    }
    case toml::node_type::string: {
        const std::string& text = node.as_string()->get();
        if (text.size() > limits::kLongTextMaxBytes || !isDisplaySafeText(text, true))
        {
            errorAt(diagnostics, node, path, "text is too long or contains control characters");
            return std::nullopt;
        }
        value.data = text;
        return value;
    }
    case toml::node_type::array: {
        Value::Array items;
        std::size_t index = 0;
        for (const auto& child : *node.as_array())
        {
            auto item = toValue(child, indexPath(path, index++), diagnostics, budget);
            if (!item)
            {
                return std::nullopt;
            }
            items.push_back(std::move(*item));
        }
        value.data = std::move(items);
        return value;
    }
    case toml::node_type::table: {
        Value::Table members;
        for (const auto& [key, child] : *node.as_table())
        {
            auto member = toValue(child, childPath(path, key.str()), diagnostics, budget);
            if (!member)
            {
                return std::nullopt;
            }
            members.push_back({std::string(key.str()), std::move(*member)});
        }
        value.data = std::move(members);
        return value;
    }
    default:
        errorAt(diagnostics, node, path, "dates and times are not supported here");
        return std::nullopt;
    }
}

void errorAt(Diagnostics& diagnostics, const toml::node& node, std::string path, std::string message)
{
    diagnostics.error(std::move(path), std::move(message), lineOf(node), columnOf(node));
}

void warningAt(Diagnostics& diagnostics, const toml::node& node, std::string path, std::string message)
{
    diagnostics.warning(std::move(path), std::move(message), lineOf(node), columnOf(node));
}

TableReader::TableReader(const toml::table& table, std::string path, Diagnostics& diagnostics)
    : m_table(table)
    , m_path(std::move(path))
    , m_diagnostics(diagnostics)
{
}

const toml::node* TableReader::get(std::string_view key)
{
    if (std::ranges::find(m_known, key) == m_known.end())
    {
        m_known.emplace_back(key);
    }
    return m_table.get(key);
}

std::optional<std::string> TableReader::string(std::string_view key, std::size_t maxBytes)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* text = node->as_string();
    if (text == nullptr)
    {
        error(key, std::format("expected a string, found {}", typeName(*node)));
        return std::nullopt;
    }
    const std::string& value = text->get();
    if (value.size() > maxBytes)
    {
        error(key, std::format("text is too long ({} bytes, limit {})", value.size(), maxBytes));
        return std::nullopt;
    }
    if (!isDisplaySafeText(value, maxBytes > limits::kShortTextMaxBytes))
    {
        error(key, "text contains control or bidirectional-override characters");
        return std::nullopt;
    }
    return value;
}

std::optional<bool> TableReader::boolean(std::string_view key)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    if (const auto* value = node->as_boolean())
    {
        return value->get();
    }
    error(key, std::format("expected true or false, found {}", typeName(*node)));
    return std::nullopt;
}

std::optional<std::int64_t> TableReader::integer(std::string_view key, std::int64_t min, std::int64_t max)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    const auto* value = node->as_integer();
    if (value == nullptr)
    {
        error(key, std::format("expected an integer, found {}", typeName(*node)));
        return std::nullopt;
    }
    if (value->get() < min || value->get() > max)
    {
        error(key, std::format("{} is out of range ({}..{})", value->get(), min, max));
        return std::nullopt;
    }
    return value->get();
}

std::optional<double> TableReader::number(std::string_view key, double min, double max)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return std::nullopt;
    }
    double number = 0.0;
    if (const auto* integer = node->as_integer())
    {
        number = static_cast<double>(integer->get());
    }
    else if (const auto* floating = node->as_floating_point())
    {
        number = floating->get();
    }
    else
    {
        error(key, std::format("expected a number, found {}", typeName(*node)));
        return std::nullopt;
    }
    if (!std::isfinite(number) || number < min || number > max)
    {
        error(key, std::format("{} is out of range ({}..{})", number, min, max));
        return std::nullopt;
    }
    return number;
}

const toml::table* TableReader::table(std::string_view key)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return nullptr;
    }
    if (const auto* value = node->as_table())
    {
        return value;
    }
    error(key, std::format("expected a table, found {}", typeName(*node)));
    return nullptr;
}

const toml::array* TableReader::array(std::string_view key, std::size_t maxElements)
{
    const toml::node* node = get(key);
    if (node == nullptr)
    {
        return nullptr;
    }
    const auto* value = node->as_array();
    if (value == nullptr)
    {
        error(key, std::format("expected an array, found {}", typeName(*node)));
        return nullptr;
    }
    if (value->size() > maxElements)
    {
        error(key, std::format("too many entries ({}, limit {})", value->size(), maxElements));
        return nullptr;
    }
    return value;
}

std::pair<std::uint32_t, std::uint32_t> TableReader::locate(std::string_view key) const
{
    if (const toml::node* node = m_table.get(key))
    {
        return {lineOf(*node), columnOf(*node)};
    }
    return {lineOf(m_table), columnOf(m_table)};
}

void TableReader::error(std::string_view key, std::string message)
{
    const auto [line, column] = locate(key);
    m_diagnostics.error(pathOf(key), std::move(message), line, column);
}

void TableReader::warning(std::string_view key, std::string message)
{
    const auto [line, column] = locate(key);
    m_diagnostics.warning(pathOf(key), std::move(message), line, column);
}

void TableReader::missing(std::string_view key)
{
    m_diagnostics.error(pathOf(key), "required key is missing", lineOf(m_table), columnOf(m_table));
}

void TableReader::reportUnknownKeys()
{
    for (const auto& [key, node] : m_table)
    {
        if (std::ranges::find(m_known, key.str()) == m_known.end())
        {
            m_diagnostics.warning(pathOf(key.str()), "unknown key (ignored) — typo or newer format?",
                                  key.source().begin.line, key.source().begin.column);
        }
    }
}

} // namespace rmw::domain::detail
