#include "rmw/bar/WidgetSource.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/domain/PathRules.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>

namespace rmw::bar
{
namespace
{

constexpr std::size_t kMaxTokens = 20'000;
constexpr std::size_t kMaxDepth = 32;
constexpr std::size_t kMaxImages = 32;
constexpr std::size_t kMaxIdentifierBytes = 64;
constexpr std::size_t kMaxStringBytes = 4'096;
constexpr std::size_t kMaxInterpolationTokens = 64;
constexpr std::int64_t kMaxRepeat = 256;

constexpr std::string_view kStdWidgets = "std-widgets.slint";

// @-keywords a widget may use. "image-url" is checked and rewritten.
constexpr auto kAllowedAtKeywords = std::to_array<std::string_view>({
    "image-url",
    "linear-gradient",
    "radial-gradient",
    "conic-gradient",
    "children",
    "tr",
});

// Elements and properties a widget may not use, written with '-' (Slint reads '_' as '-').
constexpr auto kForbiddenIdentifiers = std::to_array<std::string_view>({
    "Timer",       // the host paces updates (docs/12 §6)
    "Window",      // a bar is the only window
    "PopupWindow", // windows and popups the bar's platform does not host
    "Dialog", "ContextMenuArea", "ContextMenu", "MenuBar", "Menu", "MenuItem", "MenuSeparator",
    "iteration-count", // endless animations keep the bar redrawing
});

constexpr auto kImageExtensions = {std::string_view("png"), std::string_view("jpg"), std::string_view("jpeg"),
                                   std::string_view("svg")};

// Longest first: "<=>" before "<=" before "<".
constexpr auto kPunctuation = std::to_array<std::string_view>({
    "<=>", ":=", "=>", "->", "==", "!=", ">=", "<=", "&&", "||", "+=", "-=", "*=", "/=", "{", "}", "(", ")",
    "[",   "]",  "<",  ">",  ";",  ":",  ",",  ".",  "=",  "+",  "-",  "*",  "/",  "!",  "?", "&", "|", "%",
});

struct Token
{
    enum class Kind
    {
        Identifier,
        Number,
        Color,
        String,
        Punct,
        AtKeyword, // text: "image-url" (without '@')
    };

    Kind kind = Kind::Punct;
    std::string text;
    // String: the literal parts as written (escapes kept), and the \{ } expressions between them.
    std::vector<std::string> literals;
    std::vector<std::vector<Token>> interpolations;
    std::uint32_t line = 1;
    bool spaceBefore = false; // whitespace or a comment came before it in the source

    [[nodiscard]] bool is(std::string_view punct) const noexcept
    {
        return kind == Kind::Punct && text == punct;
    }
    [[nodiscard]] bool isWord(std::string_view word) const noexcept
    {
        return kind == Kind::Identifier && text == word;
    }
};

// Slint reads "foo_bar" and "foo-bar" as the same name.
std::string normalized(std::string_view name)
{
    std::string text(name);
    std::ranges::replace(text, '_', '-');
    return text;
}

bool startsWithRmw(std::string_view name) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return name.size() >= 3 && lower(name[0]) == 'r' && lower(name[1]) == 'm' && lower(name[2]) == 'w';
}

bool isIdentifierStart(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentifierChar(char c) noexcept
{
    return isIdentifierStart(c) || (c >= '0' && c <= '9') || c == '-';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool isHex(char c) noexcept
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

class Lexer
{
public:
    Lexer(std::string_view text, std::string_view fileName)
        : m_text(text)
        , m_file(fileName)
    {
    }

    Result<std::vector<Token>> run()
    {
        std::vector<Token> tokens;
        while (true)
        {
            auto token = next(false);
            if (!token)
            {
                return std::unexpected(std::move(token.error()));
            }
            if (!*token)
            {
                return tokens;
            }
            tokens.push_back(std::move(**token));
            if (++m_count > kMaxTokens)
            {
                return error("the file has too many tokens");
            }
        }
    }

private:
    std::unexpected<Error> error(std::string_view message) const
    {
        return fail(ErrorCode::InvalidData, std::format("{}:{}: {}", m_file, m_line, message));
    }

    // Skips whitespace and comments; true when any was there.
    Result<bool> skipSpace()
    {
        bool skipped = false;
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos];
            if (c == '\n')
            {
                ++m_line;
                ++m_pos;
            }
            else if (c == ' ' || c == '\t' || c == '\r')
            {
                ++m_pos;
            }
            else if (m_text.substr(m_pos, 2) == "//")
            {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                {
                    ++m_pos;
                }
            }
            else if (m_text.substr(m_pos, 2) == "/*")
            {
                // Comments are dropped from what is compiled, so how they nest cannot matter.
                const std::size_t end = m_text.find("*/", m_pos + 2);
                if (end == std::string_view::npos)
                {
                    return error("unterminated comment");
                }
                m_line += static_cast<std::uint32_t>(
                    std::count(m_text.begin() + static_cast<std::ptrdiff_t>(m_pos),
                               m_text.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
                m_pos = end + 2;
            }
            else
            {
                break;
            }
            skipped = true;
        }
        return skipped;
    }

    // nullopt at the end of the text (or of an interpolation, at its '}').
    Result<std::optional<Token>> next(bool interpolation)
    {
        auto space = skipSpace();
        if (!space)
        {
            return std::unexpected(std::move(space.error()));
        }
        if (m_pos >= m_text.size())
        {
            if (interpolation)
            {
                return error("unterminated \\{ in a string");
            }
            return std::optional<Token>();
        }
        Token token;
        token.line = m_line;
        token.spaceBefore = *space;
        const char c = m_text[m_pos];

        if (isIdentifierStart(c))
        {
            const std::size_t start = m_pos;
            while (m_pos < m_text.size() && isIdentifierChar(m_text[m_pos]))
            {
                ++m_pos;
            }
            token.kind = Token::Kind::Identifier;
            token.text = std::string(m_text.substr(start, m_pos - start));
            const std::string name = normalized(token.text);
            if (token.text.size() > kMaxIdentifierBytes || name.ends_with('-') ||
                name.find("--") != std::string::npos)
            {
                return error(std::format("invalid name '{}'", token.text));
            }
            return std::optional<Token>(std::move(token));
        }
        if (isDigit(c) || (c == '.' && m_pos + 1 < m_text.size() && isDigit(m_text[m_pos + 1])))
        {
            const std::size_t start = m_pos;
            while (m_pos < m_text.size() && isDigit(m_text[m_pos]))
            {
                ++m_pos;
            }
            if (m_pos + 1 < m_text.size() && m_text[m_pos] == '.' && isDigit(m_text[m_pos + 1]))
            {
                ++m_pos;
                while (m_pos < m_text.size() && isDigit(m_text[m_pos]))
                {
                    ++m_pos;
                }
            }
            // A unit: px, phx, rem, ms, s, deg, rad, turn, %.
            while (m_pos < m_text.size() &&
                   ((m_text[m_pos] >= 'a' && m_text[m_pos] <= 'z') || m_text[m_pos] == '%'))
            {
                ++m_pos;
            }
            token.kind = Token::Kind::Number;
            token.text = std::string(m_text.substr(start, m_pos - start));
            return std::optional<Token>(std::move(token));
        }
        if (c == '#')
        {
            const std::size_t start = m_pos++;
            while (m_pos < m_text.size() && isHex(m_text[m_pos]))
            {
                ++m_pos;
            }
            if (m_pos - start < 2 || (m_pos < m_text.size() && isIdentifierChar(m_text[m_pos])))
            {
                return error("invalid color");
            }
            token.kind = Token::Kind::Color;
            token.text = std::string(m_text.substr(start, m_pos - start));
            return std::optional<Token>(std::move(token));
        }
        if (c == '"')
        {
            if (interpolation)
            {
                return error("strings are not allowed inside \\{ }");
            }
            auto string = lexString(token);
            if (!string)
            {
                return std::unexpected(std::move(string.error()));
            }
            return std::optional<Token>(std::move(token));
        }
        if (c == '@')
        {
            if (interpolation)
            {
                return error("@ is not allowed inside \\{ }");
            }
            const std::size_t start = ++m_pos;
            while (m_pos < m_text.size() && isIdentifierChar(m_text[m_pos]))
            {
                ++m_pos;
            }
            token.kind = Token::Kind::AtKeyword;
            token.text = std::string(m_text.substr(start, m_pos - start));
            if (std::ranges::find(kAllowedAtKeywords, token.text) == kAllowedAtKeywords.end())
            {
                return error(std::format("@{} is not allowed in a plugin widget", token.text));
            }
            return std::optional<Token>(std::move(token));
        }
        for (const std::string_view punct : kPunctuation)
        {
            if (m_text.substr(m_pos, punct.size()) == punct)
            {
                if (interpolation && (punct == "{" || punct == "}"))
                {
                    if (punct == "}")
                    {
                        ++m_pos;
                        return std::optional<Token>(); // the end of this \{ }
                    }
                    return error("braces are not allowed inside \\{ }");
                }
                m_pos += punct.size();
                token.kind = Token::Kind::Punct;
                token.text = std::string(punct);
                return std::optional<Token>(std::move(token));
            }
        }
        return error(std::format("unexpected character '{}'", static_cast<unsigned char>(c) < 0x80
                                                                  ? std::string(1, c)
                                                                  : std::string("(non-ASCII)")));
    }

    Result<void> lexString(Token& token)
    {
        token.kind = Token::Kind::String;
        token.literals.emplace_back();
        ++m_pos; // the opening quote
        std::size_t bytes = 0;
        while (true)
        {
            if (m_pos >= m_text.size())
            {
                return error("unterminated string");
            }
            const char c = m_text[m_pos++];
            if (++bytes > kMaxStringBytes)
            {
                return error("string too long");
            }
            if (c == '"')
            {
                return {};
            }
            if (c == '\n' || c == '\r' || (static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f)
            {
                return error("control characters and line breaks are not allowed in strings");
            }
            if (c != '\\')
            {
                token.literals.back() += c;
                continue;
            }
            if (m_pos >= m_text.size())
            {
                return error("unterminated string");
            }
            const char escaped = m_text[m_pos++];
            if (escaped == '{')
            {
                std::vector<Token> expression;
                while (true)
                {
                    auto inner = next(true);
                    if (!inner)
                    {
                        return std::unexpected(std::move(inner.error()));
                    }
                    if (!*inner)
                    {
                        break;
                    }
                    if (inner->value().kind == Token::Kind::String)
                    {
                        return error("strings are not allowed inside \\{ }");
                    }
                    expression.push_back(std::move(**inner));
                    if (expression.size() > kMaxInterpolationTokens || ++m_count > kMaxTokens)
                    {
                        return error("expression in a string too long");
                    }
                }
                token.interpolations.push_back(std::move(expression));
                token.literals.emplace_back();
                continue;
            }
            if (escaped == '\n' || escaped == '\r' || static_cast<unsigned char>(escaped) < 0x20)
            {
                return error("control characters and line breaks are not allowed in strings");
            }
            token.literals.back() += '\\';
            token.literals.back() += escaped;
        }
    }

    std::string_view m_text;
    std::string_view m_file;
    std::size_t m_pos = 0;
    std::uint32_t m_line = 1;
    std::size_t m_count = 0;
};

// Writes tokens back: a space where the source had space or a comment, nothing where it had
// none, so the compiler splits them exactly as we did.
void emit(const std::vector<Token>& tokens, std::string& out)
{
    for (const Token& token : tokens)
    {
        if (token.spaceBefore && !out.empty() && out.back() != '\n')
        {
            out += ' ';
        }
        switch (token.kind)
        {
        case Token::Kind::String:
            out += '"';
            for (std::size_t i = 0; i < token.literals.size(); ++i)
            {
                out += token.literals[i];
                if (i < token.interpolations.size())
                {
                    out += "\\{";
                    emit(token.interpolations[i], out);
                    out += '}';
                }
            }
            out += '"';
            break;
        case Token::Kind::AtKeyword:
            out += '@';
            out += token.text;
            break;
        default:
            out += token.text;
            break;
        }
        if (token.is(";") || token.is("{") || token.is("}"))
        {
            out += '\n';
        }
    }
}

class Checker
{
public:
    Checker(std::vector<Token> tokens, std::string_view fileName, const WidgetSourceOptions& options)
        : m_tokens(std::move(tokens))
        , m_file(fileName)
        , m_options(options)
    {
    }

    Result<CheckedWidgetSource> run()
    {
        if (!m_options.prefix.starts_with("Rmw"))
        {
            return fail(ErrorCode::InvalidArgument, "the prefix must start with Rmw");
        }
        if (auto checked = checkTokens(m_tokens, false); !checked)
        {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto shape = readTopLevel(); !shape)
        {
            return std::unexpected(std::move(shape.error()));
        }
        CheckedWidgetSource result;
        result.stdWidgets = m_stdWidgets;
        result.names = m_names;
        for (const auto& [name, component] :
             {std::pair{m_options.component, &result.component},
              std::pair{m_options.verticalComponent, &result.verticalComponent}})
        {
            if (name.empty())
            {
                continue;
            }
            if (std::ranges::find(m_components, normalized(name)) == m_components.end())
            {
                return fail(
                    ErrorCode::InvalidData,
                    std::format("{}: the manifest names component '{}', which the file does not declare",
                                m_file, name));
            }
            *component = renamed(normalized(name));
        }
        result.interface = interfaceOf(m_options.component);
        if (!m_options.verticalComponent.empty())
        {
            result.verticalInterface = interfaceOf(m_options.verticalComponent);
        }
        renameIn(m_output);
        emit(m_output, result.source);
        return result;
    }

private:
    std::unexpected<Error> error(const Token& at, std::string_view message) const
    {
        return fail(ErrorCode::InvalidData, std::format("{}:{}: {}", m_file, at.line, message));
    }

    // Rules that hold anywhere, interpolations included.
    Result<void> checkTokens(std::vector<Token>& tokens, bool interpolation)
    {
        std::vector<char> open;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            Token& token = tokens[i];
            switch (token.kind)
            {
            case Token::Kind::Identifier: {
                const std::string name = normalized(token.text);
                if (startsWithRmw(name))
                {
                    return error(
                        token, std::format("names starting with 'Rmw' belong to the bar ('{}')", token.text));
                }
                if (std::ranges::find(kForbiddenIdentifiers, name) != kForbiddenIdentifiers.end())
                {
                    return error(token, std::format("'{}' is not allowed in a plugin widget", token.text));
                }
                // Only a statement may start with it (readTopLevel takes those out).
                const bool statementStart = i == 0 || tokens[i - 1].is(";") || tokens[i - 1].is("}");
                if (name == "import" && (interpolation || !statementStart))
                {
                    return error(token, "'import' is not allowed here");
                }
                if (name == "for")
                {
                    if (auto repeat = checkRepeater(tokens, i); !repeat)
                    {
                        return repeat;
                    }
                }
                break;
            }
            case Token::Kind::AtKeyword:
                if (token.text == "image-url")
                {
                    if (auto image = checkImage(tokens, i); !image)
                    {
                        return image;
                    }
                }
                break;
            case Token::Kind::String:
                for (auto& expression : token.interpolations)
                {
                    if (auto inner = checkTokens(expression, true); !inner)
                    {
                        return inner;
                    }
                }
                break;
            case Token::Kind::Punct:
                if (token.is("{") || token.is("(") || token.is("["))
                {
                    open.push_back(token.text[0]);
                    if (open.size() > kMaxDepth)
                    {
                        return error(token, "nested too deeply");
                    }
                }
                else if (token.is("}") || token.is(")") || token.is("]"))
                {
                    const char expected = token.is("}") ? '{' : token.is(")") ? '(' : '[';
                    if (open.empty() || open.back() != expected)
                    {
                        return error(token, std::format("unbalanced '{}'", token.text));
                    }
                    open.pop_back();
                }
                break;
            default:
                break;
            }
        }
        if (!open.empty())
        {
            return fail(ErrorCode::InvalidData, std::format("{}: unclosed '{}'", m_file, open.back()));
        }
        return {};
    }

    // for x in 1000000: Rectangle {} — a repeater with a literal count stays small.
    Result<void> checkRepeater(const std::vector<Token>& tokens, std::size_t at)
    {
        for (std::size_t i = at + 1; i < tokens.size() && i < at + 8; ++i)
        {
            if (!tokens[i].isWord("in"))
            {
                continue;
            }
            if (i + 1 < tokens.size() && tokens[i + 1].kind == Token::Kind::Number)
            {
                const std::string& text = tokens[i + 1].text;
                std::int64_t count = 0;
                const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), count);
                if (ec != std::errc{} || count > kMaxRepeat || ptr != text.data() + text.size())
                {
                    return error(tokens[i + 1],
                                 std::format("a repeater makes at most {} elements", kMaxRepeat));
                }
            }
            break;
        }
        return {};
    }

    // @image-url("icons/play.svg") -> @image-url("C:/.../icons/play.svg")
    Result<void> checkImage(std::vector<Token>& tokens, std::size_t at)
    {
        const Token& keyword = tokens[at];
        if (at + 3 >= tokens.size() || !tokens[at + 1].is("(") ||
            tokens[at + 2].kind != Token::Kind::String || !tokens[at + 3].is(")"))
        {
            return error(keyword, "@image-url takes one plain string: @image-url(\"icons/name.svg\")");
        }
        Token& path = tokens[at + 2];
        if (!path.interpolations.empty() || path.literals.size() != 1 ||
            path.literals[0].find('\\') != std::string::npos)
        {
            return error(path, "@image-url takes a plain path, without escapes or \\{ }");
        }
        if (++m_images > kMaxImages)
        {
            return error(path, std::format("at most {} images per widget", kMaxImages));
        }
        auto valid = domain::validatePackagePath(path.literals[0]);
        if (!valid)
        {
            return error(path, std::format("@image-url: {}", valid.error().message()));
        }
        if (!domain::hasExtension(*valid, kImageExtensions))
        {
            return error(path, "@image-url: expected a .png, .jpg or .svg file");
        }
        if (!m_options.resolveImage)
        {
            return error(path, "@image-url: images are not available here");
        }
        auto absolute = m_options.resolveImage(*valid);
        if (!absolute)
        {
            return error(path, std::format("@image-url(\"{}\"): {}", *valid, absolute.error().message()));
        }
        if (absolute->find_first_of("\"\\\n\r") != std::string::npos)
        {
            return error(path, "@image-url: the plugin's folder has a name the bar cannot use");
        }
        path.literals[0] = std::move(*absolute);
        return {};
    }

    // import { A, B } from "std-widgets.slint"; then declarations:
    // [export] (component | struct | enum | global) Name [inherits Base] { ... }
    Result<void> readTopLevel()
    {
        std::size_t i = 0;
        const std::size_t n = m_tokens.size();
        const auto at = [&](std::size_t k) -> const Token* { return k < n ? &m_tokens[k] : nullptr; };
        while (i < n)
        {
            const Token& first = m_tokens[i];
            if (first.is(";"))
            {
                ++i;
                continue;
            }
            if (first.isWord("import"))
            {
                auto end = readImport(i);
                if (!end)
                {
                    return std::unexpected(std::move(end.error()));
                }
                i = *end;
                continue;
            }
            const std::size_t start = i;
            if (first.isWord("export"))
            {
                ++i;
                const Token* next = at(i);
                if (next == nullptr || next->kind != Token::Kind::Identifier ||
                    !(next->text == "component" || next->text == "struct" || next->text == "enum" ||
                      next->text == "global"))
                {
                    return error(
                        first, "only components, structs, enums and globals can be exported (no re-exports)");
                }
            }
            const Token& keyword = m_tokens[i];
            if (!(keyword.isWord("component") || keyword.isWord("struct") || keyword.isWord("enum") ||
                  keyword.isWord("global")))
            {
                return error(keyword, "expected a component, struct, enum or global declaration");
            }
            const Token* name = at(i + 1);
            if (name == nullptr || name->kind != Token::Kind::Identifier)
            {
                return error(keyword, "expected a name");
            }
            const std::string declared = normalized(name->text);
            if (std::ranges::find(m_names, declared, &std::pair<std::string, std::string>::first) !=
                m_names.end())
            {
                return error(*name, std::format("'{}' is declared twice", name->text));
            }
            m_names.emplace_back(declared, std::format("{}-{}", m_options.prefix, declared));
            if (keyword.isWord("component"))
            {
                m_components.push_back(declared);
            }
            i += 2;
            if (const Token* inherits = at(i); inherits != nullptr && inherits->isWord("inherits"))
            {
                const Token* base = at(i + 1);
                if (base == nullptr || base->kind != Token::Kind::Identifier)
                {
                    return error(*inherits, "expected what the component inherits");
                }
                i += 2;
            }
            if (const Token* old = at(i); old != nullptr && old->is(":="))
            {
                ++i; // struct Name := { ... } (older syntax)
            }
            const Token* open = at(i);
            if (open == nullptr || !open->is("{"))
            {
                return error(open != nullptr ? *open : keyword, "expected '{'");
            }
            const std::size_t bodyStart = i;
            int depth = 0;
            for (; i < n; ++i)
            {
                depth += m_tokens[i].is("{") ? 1 : m_tokens[i].is("}") ? -1 : 0;
                if (depth == 0)
                {
                    break;
                }
            }
            if (keyword.isWord("component"))
            {
                m_bodies.emplace_back(declared, std::pair{bodyStart, i});
            }
            ++i; // past the closing brace
            m_output.insert(m_output.end(), m_tokens.begin() + static_cast<std::ptrdiff_t>(start),
                            m_tokens.begin() + static_cast<std::ptrdiff_t>(std::min(i, n)));
        }
        return {};
    }

    // import { A, B } from "std-widgets.slint";  -> remembered, left out of the output.
    Result<std::size_t> readImport(std::size_t i)
    {
        const Token& keyword = m_tokens[i];
        const auto fault = [&] {
            return error(keyword,
                         "only `import { Name, ... } from \"std-widgets.slint\";` is allowed (files and "
                         "fonts stay out of plugin widgets)");
        };
        const std::size_t n = m_tokens.size();
        if (i + 1 >= n || !m_tokens[i + 1].is("{"))
        {
            return fault();
        }
        i += 2;
        std::vector<std::string> names;
        while (i < n && !m_tokens[i].is("}"))
        {
            if (m_tokens[i].kind != Token::Kind::Identifier || !isIdentifierStart(m_tokens[i].text[0]) ||
                m_tokens[i].text[0] < 'A' || m_tokens[i].text[0] > 'Z')
            {
                return fault();
            }
            names.push_back(m_tokens[i].text);
            ++i;
            if (i < n && m_tokens[i].is(","))
            {
                ++i;
            }
        }
        if (i + 2 >= n || !m_tokens[i].is("}") || !m_tokens[i + 1].isWord("from") ||
            m_tokens[i + 2].kind != Token::Kind::String || m_tokens[i + 2].literals.size() != 1 ||
            m_tokens[i + 2].literals[0] != kStdWidgets || names.empty())
        {
            return fault();
        }
        i += 3;
        if (i < n && m_tokens[i].is(";"))
        {
            ++i;
        }
        for (auto& name : names)
        {
            if (std::ranges::find(m_stdWidgets, name) == m_stdWidgets.end())
            {
                m_stdWidgets.push_back(std::move(name));
            }
        }
        return i;
    }

    // The in/in-out properties and callbacks a component declares at its own level.
    WidgetInterface interfaceOf(std::string_view component) const
    {
        WidgetInterface found;
        const auto body =
            std::ranges::find(m_bodies, normalized(component),
                              &std::pair<std::string, std::pair<std::size_t, std::size_t>>::first);
        if (body == m_bodies.end())
        {
            return found;
        }
        const auto [first, last] = body->second;
        int depth = 0;
        for (std::size_t i = first; i <= last && i < m_tokens.size(); ++i)
        {
            const Token& token = m_tokens[i];
            if (token.is("{") || token.is("(") || token.is("["))
            {
                ++depth;
                continue;
            }
            if (token.is("}") || token.is(")") || token.is("]"))
            {
                --depth;
                continue;
            }
            if (depth != 1)
            {
                continue;
            }
            const bool input = token.kind == Token::Kind::Identifier &&
                               (token.text == "in" || normalized(token.text) == "in-out");
            if (input && i + 1 < m_tokens.size() && m_tokens[i + 1].isWord("property"))
            {
                if (auto name = propertyName(i + 2))
                {
                    found.inProperties.push_back(normalized(*name));
                }
            }
            else if (token.isWord("callback") && i + 1 < m_tokens.size() &&
                     m_tokens[i + 1].kind == Token::Kind::Identifier)
            {
                found.callbacks.push_back(normalized(m_tokens[i + 1].text));
            }
        }
        return found;
    }

    // After `property`: `<type> name`, the type possibly nested ("<[{a: int}]>").
    std::optional<std::string> propertyName(std::size_t i) const
    {
        if (i >= m_tokens.size() || !m_tokens[i].is("<"))
        {
            return std::nullopt;
        }
        int angle = 0;
        for (; i < m_tokens.size(); ++i)
        {
            const Token& token = m_tokens[i];
            angle += token.is("<") ? 1 : token.is(">") ? -1 : 0;
            if (angle == 0)
            {
                break;
            }
        }
        if (i + 1 < m_tokens.size() && m_tokens[i + 1].kind == Token::Kind::Identifier)
        {
            return m_tokens[i + 1].text;
        }
        return std::nullopt;
    }

    std::string renamed(const std::string& normalizedName) const
    {
        const auto found =
            std::ranges::find(m_names, normalizedName, &std::pair<std::string, std::string>::first);
        return found == m_names.end() ? normalizedName : found->second;
    }

    // Every use of a top-level name, except after '.' (a member that happens to share it).
    void renameIn(std::vector<Token>& tokens)
    {
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            Token& token = tokens[i];
            if (token.kind == Token::Kind::String)
            {
                for (auto& expression : token.interpolations)
                {
                    renameIn(expression);
                }
                continue;
            }
            if (token.kind != Token::Kind::Identifier || (i > 0 && tokens[i - 1].is(".")))
            {
                continue;
            }
            const std::string name = normalized(token.text);
            const auto found = std::ranges::find(m_names, name, &std::pair<std::string, std::string>::first);
            if (found != m_names.end())
            {
                token.text = found->second;
            }
        }
    }

    std::vector<Token> m_tokens;
    std::string_view m_file;
    const WidgetSourceOptions& m_options;
    std::vector<Token> m_output;
    std::vector<std::string> m_stdWidgets;
    std::vector<std::pair<std::string, std::string>> m_names; // normalized -> renamed
    std::vector<std::string> m_components;                    // normalized
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> m_bodies;
    std::size_t m_images = 0;
};

} // namespace

std::string CheckedWidgetSource::renamed(std::string_view name) const
{
    const std::string key = normalized(name);
    const auto found = std::ranges::find(names, key, &std::pair<std::string, std::string>::first);
    return found == names.end() ? std::string(name) : found->second;
}

Result<CheckedWidgetSource> checkWidgetSource(std::string_view text, std::string_view fileName,
                                              const WidgetSourceOptions& options)
{
    if (text.size() > kWidgetSourceMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{}: larger than {} KiB", fileName, kWidgetSourceMaxBytes >> 10));
    }
    if (!isValidUtf8(text))
    {
        return fail(ErrorCode::InvalidData, std::format("{}: not valid UTF-8", fileName));
    }
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3); // a byte-order mark
    }
    auto tokens = Lexer(text, fileName).run();
    if (!tokens)
    {
        return std::unexpected(std::move(tokens.error()));
    }
    return Checker(std::move(*tokens), fileName, options).run();
}

} // namespace rmw::bar
