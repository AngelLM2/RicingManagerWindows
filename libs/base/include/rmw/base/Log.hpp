#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/base/Limits.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Logging (docs/08, docs/06 §8): UTF-8 lines, one file per process, rotation, path redaction.
// Logging never throws: a failure to log must not become a failure of the program.
namespace rmw::log
{

enum class Level : std::uint8_t
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

std::string_view toString(Level level) noexcept;

// Destination for formatted lines (without trailing newline).
class ISink
{
public:
    virtual ~ISink() = default;
    virtual void write(Level level, std::string_view line) noexcept = 0;
    virtual void flush() noexcept {}
};

struct RotatingFileOptions
{
    std::filesystem::path path; // e.g. %LOCALAPPDATA%\RicingManagerWindows\logs\core.log
    std::uint64_t maxBytes = limits::kLogFileMaxBytes;
    std::uint32_t maxFiles = limits::kLogMaxFiles; // including the active file
};

// core.log, core.1.log, ... core.(maxFiles-1).log. Creates the directory if needed.
Result<std::unique_ptr<ISink>> makeRotatingFileSink(RotatingFileOptions options);

class Logger
{
public:
    void setLevel(Level level) noexcept;
    Level level() const noexcept;
    bool isEnabled(Level level) const noexcept;

    void addSink(std::unique_ptr<ISink> sink);
    void clearSinks() noexcept;

    // Every occurrence of `from` (ASCII case-insensitive) is replaced by `to` before writing,
    // e.g. "C:\Users\joao" -> "%USERPROFILE%" (docs/06 §8).
    void addRedaction(std::string from, std::string to);

    // Formats the final line (timestamp, level, thread, location) and sends it to the sinks.
    void write(Level level, std::string_view message, const std::source_location& where) noexcept;
    void flush() noexcept;

private:
    mutable std::mutex m_mutex;
    std::atomic<Level> m_level{Level::Info};
    std::vector<std::unique_ptr<ISink>> m_sinks;
    std::vector<std::pair<std::string, std::string>> m_redactions;
};

// The process-wide logger: the only global mutable state allowed (docs/08 §3 rule 10).
Logger& instance() noexcept;

// Format string that also captures the caller's location.
template <class... Args>
struct FormatAt
{
    template <class S>
        requires std::convertible_to<const S&, std::string_view>
    consteval FormatAt(const S& text, std::source_location where = std::source_location::current())
        : format(text)
        , location(where)
    {
    }

    std::format_string<Args...> format;
    std::source_location location;
};

template <class... Args>
void write(Level level, FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    Logger& logger = instance();
    if (!logger.isEnabled(level))
    {
        return;
    }
    try
    {
        logger.write(level, std::format(fmt.format, std::forward<Args>(args)...), fmt.location);
    }
    catch (...)
    {
        // Out of memory while formatting: drop the line.
    }
}

template <class... Args>
void trace(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Warn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Error, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void critical(FormatAt<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept
{
    write<Args...>(Level::Critical, fmt, std::forward<Args>(args)...);
}

} // namespace rmw::log
