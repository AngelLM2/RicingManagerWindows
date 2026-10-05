#include "rmw/base/Log.hpp"

#include <fstream>
#include <system_error>

namespace rmw::log
{
namespace
{

class RotatingFileSink final : public ISink
{
public:
    explicit RotatingFileSink(RotatingFileOptions options)
        : m_options(std::move(options))
    {
        open();
    }

    void write(Level level, std::string_view line) noexcept override
    {
        try
        {
            const std::uint64_t needed = line.size() + 1;
            if (m_size > 0 && m_size + needed > m_options.maxBytes)
            {
                rotate();
            }
            if (!m_stream.is_open() || !m_stream.good())
            {
                open();
                if (!m_stream.is_open())
                {
                    return;
                }
            }

            m_stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            m_stream.put('\n');
            m_size += needed;

            // Flush everything from Warn up immediately: those lines matter after a crash.
            if (level >= Level::Warn)
            {
                m_stream.flush();
            }
        }
        catch (...)
        {
            // Logging never throws.
        }
    }

    void flush() noexcept override
    {
        try
        {
            if (m_stream.is_open())
            {
                m_stream.flush();
            }
        }
        catch (...)
        {
        }
    }

private:
    std::filesystem::path rotatedPath(std::uint32_t index) const
    {
        std::filesystem::path path = m_options.path;
        path.replace_extension(std::to_wstring(index) + m_options.path.extension().wstring());
        return path;
    }

    void open()
    {
        m_stream.close();
        m_stream.clear();
        m_stream.open(m_options.path, std::ios::binary | std::ios::app);

        std::error_code ec;
        const auto size = std::filesystem::file_size(m_options.path, ec);
        m_size = ec ? 0 : size;
    }

    void rotate()
    {
        m_stream.close();
        std::error_code ec;
        if (m_options.maxFiles <= 1)
        {
            std::filesystem::remove(m_options.path, ec);
        }
        else
        {
            // core.(N-1).log is dropped, every other file moves one slot up.
            std::filesystem::remove(rotatedPath(m_options.maxFiles - 1), ec);
            for (std::uint32_t index = m_options.maxFiles - 1; index > 1; --index)
            {
                std::filesystem::rename(rotatedPath(index - 1), rotatedPath(index), ec);
            }
            std::filesystem::rename(m_options.path, rotatedPath(1), ec);
        }
        open();
    }

    RotatingFileOptions m_options;
    std::ofstream m_stream;
    std::uint64_t m_size = 0;
};

} // namespace

Result<std::unique_ptr<ISink>> makeRotatingFileSink(RotatingFileOptions options)
{
    if (options.path.empty() || !options.path.has_filename())
    {
        return fail(ErrorCode::InvalidArgument, "log file path must name a file");
    }
    if (options.maxBytes == 0 || options.maxFiles == 0)
    {
        return fail(ErrorCode::InvalidArgument, "log rotation limits must be positive");
    }

    std::error_code ec;
    std::filesystem::create_directories(options.path.parent_path(), ec);
    if (ec)
    {
        return fail(ErrorCode::Io, "cannot create log directory: " + ec.message(), ec.value());
    }

    auto sink = std::make_unique<RotatingFileSink>(std::move(options));
    return std::unique_ptr<ISink>(std::move(sink));
}

} // namespace rmw::log
