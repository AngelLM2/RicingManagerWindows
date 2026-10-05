#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>

namespace rmw
{

// Error categories shared by every layer. OS- or library-specific details go in nativeCode().
enum class ErrorCode : std::uint16_t
{
    Unknown = 1,
    InvalidArgument, // caller passed something invalid
    InvalidData,     // malformed input: encoding, parse, schema
    NotFound,
    AlreadyExists,
    AccessDenied,
    Busy,
    Timeout,
    Cancelled,
    LimitExceeded, // an input limit from Limits.hpp was hit
    Unsupported,   // not available on this Windows version / configuration
    Io,
    System,   // nativeCode() holds a Win32 error or an HRESULT
    Database, // nativeCode() holds an SQLite extended result code
};

std::string_view toString(ErrorCode code) noexcept;

// Value type describing a failure. Messages are UTF-8.
class Error
{
public:
    Error(ErrorCode code, std::string message, std::int64_t nativeCode = 0,
          std::source_location where = std::source_location::current());

    ErrorCode code() const noexcept { return m_code; }
    const std::string& message() const noexcept { return m_message; }
    std::int64_t nativeCode() const noexcept { return m_nativeCode; }
    const std::source_location& where() const noexcept { return m_where; }

    // Prepends context: "loading rice" + "file not found" -> "loading rice: file not found".
    Error& withContext(std::string_view context) &;
    Error&& withContext(std::string_view context) &&;

    // "NotFound: loading rice: file not found (native 0x00000002) [RiceLoader.cpp:42]"
    std::string toString() const;

private:
    ErrorCode m_code;
    std::string m_message;
    std::int64_t m_nativeCode;
    std::source_location m_where;
};

template <class T>
using Result = std::expected<T, Error>;

// return rmw::fail(ErrorCode::NotFound, "rice.toml not found");
inline std::unexpected<Error> fail(ErrorCode code, std::string message, std::int64_t nativeCode = 0,
                                   std::source_location where = std::source_location::current())
{
    return std::unexpected<Error>(Error(code, std::move(message), nativeCode, where));
}

} // namespace rmw
