#pragma once

#include <string_view>

// Syntax of the names authors write in rice.toml and plugin.toml.
namespace rmw::domain
{

// Reverse-DNS package id: "io.github.fulano.now-playing". At least two segments; each segment
// is [a-z0-9] followed by [a-z0-9-], no leading/trailing '-'; at most 128 bytes.
bool isValidPackageId(std::string_view id) noexcept;

// Built-in widget/action/provider name: "clock", "tasklist", "media.control". Lower-case
// [a-z][a-z0-9-]* segments joined by '.', at most 64 bytes.
bool isValidBuiltinName(std::string_view name) noexcept;

// Palette color name: [a-z][a-z0-9_-]*, at most 32 bytes.
bool isValidPaletteName(std::string_view name) noexcept;

} // namespace rmw::domain
