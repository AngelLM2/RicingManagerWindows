#pragma once

#include "rmw/base/Error.hpp"

#include <string>
#include <string_view>

// Rules for paths that must stay inside a package (rice or plugin) — docs/06 §4. Used by the
// rice/plugin parsers, the package importer and the plugin .slint validator. Pure and fuzzable.
namespace rmw::domain
{

// Accepts a relative path made of safe components and returns it normalized with '/'
// separators. Rejects: empty, absolute, drive letters, UNC and \\?\ prefixes, "." / ".." / empty
// components, < > : " | ? * and control characters, components ending in '.' or ' ', reserved
// device names (CON, PRN, AUX, NUL, COM0-9, LPT0-9, COM¹²³, LPT¹²³ — with or without extension),
// 8.3 short-name aliases ("~" followed by a digit), invalid UTF-8 and over-long names.
Result<std::string> validatePackagePath(std::string_view path);

// True when the path's extension (case-insensitive, without dot) is one of `allowed`,
// e.g. hasExtension("a/b.CUR", {"cur", "ani"}).
bool hasExtension(std::string_view path, std::initializer_list<std::string_view> allowed) noexcept;

} // namespace rmw::domain
