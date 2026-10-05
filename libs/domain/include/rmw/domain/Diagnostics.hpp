#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rmw::domain
{

enum class Severity
{
    Warning,
    Error,
};

// A problem found while reading user-authored data (rice.toml, plugin.toml), with the location
// to show the author: "rice.toml:12:9: [wallpaper.monitor[1].fit] unknown fit 'zoom'".
struct Diagnostic
{
    Severity severity = Severity::Error;
    std::string message;
    std::string key; // dotted key path, e.g. "wallpaper.monitor[1].fit"
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    std::string toString(std::string_view source = {}) const;
};

class Diagnostics
{
public:
    void error(std::string key, std::string message, std::uint32_t line = 0, std::uint32_t column = 0);
    void warning(std::string key, std::string message, std::uint32_t line = 0, std::uint32_t column = 0);

    bool hasErrors() const noexcept;
    const std::vector<Diagnostic>& all() const noexcept { return m_items; }

    // All errors joined, for a single-line summary.
    std::string errorSummary(std::string_view source = {}) const;

private:
    std::vector<Diagnostic> m_items;
};

} // namespace rmw::domain
