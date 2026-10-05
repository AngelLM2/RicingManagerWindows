#include "rmw/domain/Diagnostics.hpp"

#include <algorithm>
#include <format>

namespace rmw::domain
{

std::string Diagnostic::toString(std::string_view source) const
{
    std::string text;
    if (!source.empty())
    {
        text += source;
        if (line > 0)
        {
            text += std::format(":{}:{}", line, column);
        }
        text += ": ";
    }
    text += severity == Severity::Error ? "error" : "warning";
    if (!key.empty())
    {
        text += std::format(" [{}]", key);
    }
    text += ": " + message;
    return text;
}

void Diagnostics::error(std::string key, std::string message, std::uint32_t line, std::uint32_t column)
{
    m_items.push_back({Severity::Error, std::move(message), std::move(key), line, column});
}

void Diagnostics::warning(std::string key, std::string message, std::uint32_t line, std::uint32_t column)
{
    m_items.push_back({Severity::Warning, std::move(message), std::move(key), line, column});
}

bool Diagnostics::hasErrors() const noexcept
{
    return std::ranges::any_of(m_items, [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

std::string Diagnostics::errorSummary(std::string_view source) const
{
    std::string text;
    for (const auto& item : m_items)
    {
        if (item.severity == Severity::Error)
        {
            if (!text.empty())
            {
                text += "; ";
            }
            text += item.toString(source);
        }
    }
    return text;
}

} // namespace rmw::domain
