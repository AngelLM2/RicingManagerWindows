#include "rmw/base/Log.hpp"

#include <chrono>
#include <thread>

namespace rmw::log
{
namespace
{

char asciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::size_t findCaseInsensitive(std::string_view haystack, std::string_view needle, std::size_t from) noexcept
{
    if (needle.empty() || needle.size() > haystack.size())
    {
        return std::string_view::npos;
    }
    for (std::size_t pos = from; pos + needle.size() <= haystack.size(); ++pos)
    {
        bool match = true;
        for (std::size_t i = 0; i < needle.size(); ++i)
        {
            if (asciiLower(haystack[pos + i]) != asciiLower(needle[i]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return pos;
        }
    }
    return std::string_view::npos;
}

void replaceAllCaseInsensitive(std::string& text, std::string_view from, std::string_view to)
{
    for (std::size_t pos = findCaseInsensitive(text, from, 0); pos != std::string::npos;
         pos = findCaseInsensitive(text, from, pos + to.size()))
    {
        text.replace(pos, from.size(), to);
    }
}

// One log entry must stay one line: escape CR/LF so input cannot forge extra entries.
std::string sanitize(std::string_view message)
{
    std::string out;
    out.reserve(message.size());
    for (const char c : message)
    {
        if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else
        {
            out.push_back(c);
        }
    }

    if (out.size() > limits::kLogMessageMaxBytes)
    {
        std::size_t cut = limits::kLogMessageMaxBytes;
        // Do not split a UTF-8 sequence: back up to a lead byte.
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }
        out.resize(cut);
        out += " [truncated]";
    }
    return out;
}

std::string_view fileName(const std::source_location& where) noexcept
{
    std::string_view file = where.file_name();
    if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos)
    {
        file.remove_prefix(slash + 1);
    }
    return file;
}

} // namespace

std::string_view toString(Level level) noexcept
{
    switch (level)
    {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRIT";
    case Level::Off:
        return "OFF";
    }
    return "?";
}

void Logger::setLevel(Level level) noexcept
{
    m_level.store(level, std::memory_order_relaxed);
}

Level Logger::level() const noexcept
{
    return m_level.load(std::memory_order_relaxed);
}

bool Logger::isEnabled(Level level) const noexcept
{
    const Level current = this->level();
    return current != Level::Off && level >= current;
}

void Logger::addSink(std::unique_ptr<ISink> sink)
{
    if (!sink)
    {
        return;
    }
    const std::lock_guard lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::clearSinks() noexcept
{
    const std::lock_guard lock(m_mutex);
    for (auto& sink : m_sinks)
    {
        sink->flush();
    }
    m_sinks.clear();
}

void Logger::addRedaction(std::string from, std::string to)
{
    if (from.empty())
    {
        return;
    }
    const std::lock_guard lock(m_mutex);
    m_redactions.emplace_back(std::move(from), std::move(to));
}

void Logger::write(Level level, std::string_view message, const std::source_location& where) noexcept
{
    if (!isEnabled(level))
    {
        return;
    }
    try
    {
        // Build the line outside the lock; only redaction and sink I/O happen inside.
        std::string line = std::format(
            "{:%FT%TZ} {:<5} [{}] {} ({}:{})",
            std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()), toString(level),
            std::this_thread::get_id(), sanitize(message), fileName(where), where.line());

        const std::lock_guard lock(m_mutex);
        for (const auto& [from, to] : m_redactions)
        {
            replaceAllCaseInsensitive(line, from, to);
        }
        for (auto& sink : m_sinks)
        {
            sink->write(level, line);
        }
    }
    catch (...)
    {
        // Logging never throws.
    }
}

void Logger::flush() noexcept
{
    const std::lock_guard lock(m_mutex);
    for (auto& sink : m_sinks)
    {
        sink->flush();
    }
}

Logger& instance() noexcept
{
    static Logger logger;
    return logger;
}

} // namespace rmw::log
