#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

// Human-readable text and rmw::Error values for Win32 errors and HRESULTs.
// The header stays free of <windows.h>: DWORD is std::uint32_t, HRESULT is std::int32_t.
namespace rmw::platform
{

// System message for a Win32 error code, UTF-8, without trailing line breaks.
// Never reads insertion arguments (FORMAT_MESSAGE_IGNORE_INSERTS — docs/02 C5).
std::string win32Message(std::uint32_t errorCode);

// System message for an HRESULT (Win32-facility HRESULTs are unwrapped first).
std::string hresultMessage(std::int32_t hr);

// rmw::Error with ErrorCode::System (or AccessDenied/NotFound when they map clearly).
Error win32Error(std::uint32_t errorCode, std::string_view context,
                 std::source_location where = std::source_location::current());
Error hresultError(std::int32_t hr, std::string_view context,
                   std::source_location where = std::source_location::current());

// win32Error(GetLastError(), context). Call it immediately after the failing API.
Error lastError(std::string_view context, std::source_location where = std::source_location::current());

} // namespace rmw::platform
