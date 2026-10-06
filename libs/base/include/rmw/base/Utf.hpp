#pragma once

#include "rmw/base/Error.hpp"

#include <string>
#include <string_view>

// UTF-8 <-> UTF-16 conversion (docs/08 §3 rule 6): UTF-16 only at the Win32 boundary,
// UTF-8 everywhere else. Implemented without Windows APIs so it is portable and fuzzable.
namespace rmw
{

static_assert(sizeof(wchar_t) == 2, "rmw assumes the Windows ABI: wchar_t is a UTF-16 code unit");

// Strict: fails on malformed input (overlong forms, surrogates, truncated sequences,
// code points above U+10FFFF). The error message carries the offending offset.
Result<std::wstring> utf8To16(std::string_view utf8);
Result<std::string> utf16To8(std::wstring_view utf16);

// Lossy: replaces each malformed unit with U+FFFD. Use only where data loss is acceptable
// (logs, diagnostics), never for paths or registry values.
std::wstring utf8To16Lossy(std::string_view utf8);
std::string utf16To8Lossy(std::wstring_view utf16);

bool isValidUtf8(std::string_view text) noexcept;

// The longest start of `text` (valid UTF-8) of at most `maxBytes` that ends between two characters: a cut
// in the middle of one is invalid UTF-8, which Slint aborts on.
std::string_view utf8Prefix(std::string_view text, std::size_t maxBytes) noexcept;

} // namespace rmw
