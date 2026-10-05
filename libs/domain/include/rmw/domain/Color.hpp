#pragma once

#include "rmw/base/Error.hpp"

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace rmw::domain
{

// sRGB color with alpha. Text form: "#RGB", "#RRGGBB" or "#RRGGBBAA".
struct Color
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    static Result<Color> parse(std::string_view text);

    // "#RRGGBB" when opaque, "#RRGGBBAA" otherwise.
    std::string toHex() const;

    friend auto operator<=>(const Color&, const Color&) = default;
};

} // namespace rmw::domain
