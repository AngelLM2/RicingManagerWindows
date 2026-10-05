#include "rmw/platform/WinError.hpp"

#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <format>

namespace rmw::platform
{
namespace
{

std::string formatSystemMessage(DWORD code)
{
    wil::unique_hlocal_string buffer;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(buffer.put()), 0, nullptr);

    if (length == 0 || !buffer)
    {
        return std::format("unknown error 0x{:08X}", static_cast<std::uint32_t>(code));
    }

    std::wstring_view text(buffer.get(), length);
    while (!text.empty() &&
           (text.back() == L' ' || text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.'))
    {
        text.remove_suffix(1);
    }
    return utf16To8Lossy(text);
}

ErrorCode classifyWin32(DWORD code) noexcept
{
    switch (code)
    {
    case ERROR_ACCESS_DENIED:
        return ErrorCode::AccessDenied;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ErrorCode::NotFound;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return ErrorCode::AlreadyExists;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT:
        return ErrorCode::Timeout;
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:
        return ErrorCode::Busy;
    case ERROR_CANCELLED:
        return ErrorCode::Cancelled;
    case ERROR_INVALID_PARAMETER:
        return ErrorCode::InvalidArgument;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return ErrorCode::Unsupported;
    default:
        return ErrorCode::System;
    }
}

} // namespace

std::string win32Message(std::uint32_t errorCode)
{
    return formatSystemMessage(errorCode);
}

std::string hresultMessage(std::int32_t hr)
{
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
    {
        return formatSystemMessage(HRESULT_CODE(hr));
    }
    return formatSystemMessage(static_cast<DWORD>(hr));
}

Error win32Error(std::uint32_t errorCode, std::string_view context, std::source_location where)
{
    return Error(classifyWin32(errorCode), win32Message(errorCode), errorCode, where).withContext(context);
}

Error hresultError(std::int32_t hr, std::string_view context, std::source_location where)
{
    const ErrorCode code =
        HRESULT_FACILITY(hr) == FACILITY_WIN32 ? classifyWin32(HRESULT_CODE(hr)) : ErrorCode::System;
    return Error(code, hresultMessage(hr), static_cast<std::uint32_t>(hr), where).withContext(context);
}

Error lastError(std::string_view context, std::source_location where)
{
    const DWORD code = GetLastError();
    return win32Error(code, context, where);
}

} // namespace rmw::platform
