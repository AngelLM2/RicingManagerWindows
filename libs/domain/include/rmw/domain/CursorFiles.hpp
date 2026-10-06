#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// Cursor packages are untrusted and, once applied, every process on the desktop reads the files
// (docs/06 §4). Both functions are bounded, allocation-light parsers and fuzzing targets.
namespace rmw::domain
{

enum class CursorFormat
{
    Static,   // .cur (ICONDIR type 2)
    Animated, // .ani (RIFF ACON)
};

// Checks the structure of a .cur/.ani file: headers, directory entries and chunk sizes inside the
// file, sane dimensions and frame counts, every embedded frame a valid icon/cursor. Does not
// decode pixels. `extension` ("cur" or "ani") must match the content.
[[nodiscard]] Result<CursorFormat> validateCursorFile(std::span<const std::byte> bytes,
                                                      std::string_view extension);

// The same checks for an .ico (an icon directory, type 1) of an icon pack (M14): once set, Explorer
// and every app showing a file dialog read it.
[[nodiscard]] Result<void> validateIconFile(std::span<const std::byte> bytes);

// Reads the cursor scheme of an install.inf (the format cursor packs ship with): the value list
// written to HKCU\Control Panel\Cursors\Schemes, with %strings% substituted, mapped to roles in
// Windows' order. Returns file names (no folders); each passed PathRules and is .cur or .ani.
[[nodiscard]] Result<std::vector<CursorAssignment>> parseCursorInf(std::string_view text);

} // namespace rmw::domain
