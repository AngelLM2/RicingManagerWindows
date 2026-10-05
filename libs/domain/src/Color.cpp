#include "rmw/domain/Color.hpp"

#include <array>
#include <format>
#include <optional>

namespace rmw::domain
{
namespace
{

std::optional<std::uint8_t> hexDigit(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f')
    {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F')
    {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return std::nullopt;
}

} // namespace

Result<Color> Color::parse(std::string_view text)
{
    const auto invalid = [&] {
        return fail(ErrorCode::InvalidData,
                    std::format("invalid color '{}' (expected #RGB, #RRGGBB or #RRGGBBAA)", text));
    };
    if (text.empty() || text.front() != '#')
    {
        return invalid();
    }
    const std::string_view digits = text.substr(1);
    if (digits.size() != 3 && digits.size() != 6 && digits.size() != 8)
    {
        return invalid();
    }

    std::array<std::uint8_t, 8> values{};
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        const auto value = hexDigit(digits[i]);
        if (!value)
        {
            return invalid();
        }
        values[i] = *value;
    }

    Color color;
    if (digits.size() == 3)
    {
        color.r = static_cast<std::uint8_t>(values[0] * 17);
        color.g = static_cast<std::uint8_t>(values[1] * 17);
        color.b = static_cast<std::uint8_t>(values[2] * 17);
        return color;
    }
    color.r = static_cast<std::uint8_t>(values[0] * 16 + values[1]);
    color.g = static_cast<std::uint8_t>(values[2] * 16 + values[3]);
    color.b = static_cast<std::uint8_t>(values[4] * 16 + values[5]);
    if (digits.size() == 8)
    {
        color.a = static_cast<std::uint8_t>(values[6] * 16 + values[7]);
    }
    return color;
}

std::string Color::toHex() const
{
    if (a == 255)
    {
        return std::format("#{:02X}{:02X}{:02X}", r, g, b);
    }
    return std::format("#{:02X}{:02X}{:02X}{:02X}", r, g, b, a);
}

} // namespace rmw::domain
