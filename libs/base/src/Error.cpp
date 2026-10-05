#include "rmw/base/Error.hpp"

#include <format>

namespace rmw
{

std::string_view toString(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::Unknown:
        return "Unknown";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::InvalidData:
        return "InvalidData";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::AccessDenied:
        return "AccessDenied";
    case ErrorCode::Busy:
        return "Busy";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::LimitExceeded:
        return "LimitExceeded";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::Io:
        return "Io";
    case ErrorCode::System:
        return "System";
    case ErrorCode::Database:
        return "Database";
    }
    return "Unknown";
}

Error::Error(ErrorCode code, std::string message, std::int64_t nativeCode, std::source_location where)
    : m_code(code)
    , m_message(std::move(message))
    , m_nativeCode(nativeCode)
    , m_where(where)
{
}

Error& Error::withContext(std::string_view context) &
{
    if (!context.empty())
    {
        m_message = std::format("{}: {}", context, m_message);
    }
    return *this;
}

Error&& Error::withContext(std::string_view context) &&
{
    withContext(context);
    return std::move(*this);
}

std::string Error::toString() const
{
    // Only the file name: full build paths are noise in logs.
    std::string_view file = m_where.file_name();
    if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos)
    {
        file.remove_prefix(slash + 1);
    }

    if (m_nativeCode != 0)
    {
        return std::format("{}: {} (native 0x{:08X}) [{}:{}]", rmw::toString(m_code), m_message,
                           static_cast<std::uint32_t>(m_nativeCode), file, m_where.line());
    }
    return std::format("{}: {} [{}:{}]", rmw::toString(m_code), m_message, file, m_where.line());
}

} // namespace rmw
