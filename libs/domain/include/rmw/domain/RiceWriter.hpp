#pragma once

#include "rmw/domain/Rice.hpp"
#include "rmw/domain/Value.hpp"

#include <string>
#include <string_view>

// rice.toml from the model (docs/04 M7: the settings app edits a Rice and writes it back). The text
// parses back to the same rice, and writing that again gives the same text (the tests and the
// rice_toml fuzz target check both). Comments and unknown keys of an edited file are not kept.
namespace rmw::domain
{

[[nodiscard]] std::string writeRice(const Rice& rice);

// A TOML basic string ("...") with every escape TOML requires.
[[nodiscard]] std::string tomlQuote(std::string_view text);
// A key as written: bare when it can be (letters, digits, '-', '_'), quoted otherwise.
[[nodiscard]] std::string tomlKey(std::string_view key);
// A value inline: strings quoted, floats always with a '.', tables { k = v }; null (which TOML
// cannot hold) is written as an empty table's absence by the callers, never here.
[[nodiscard]] std::string tomlValue(const Value& value);

} // namespace rmw::domain
