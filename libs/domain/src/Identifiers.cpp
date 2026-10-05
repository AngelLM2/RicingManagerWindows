#include "rmw/domain/Identifiers.hpp"

namespace rmw::domain
{
namespace
{

bool isLowerAlpha(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// Splits on '.' and checks every segment with `segmentOk`; returns the segment count (0 = bad).
template <class SegmentOk>
std::size_t checkSegments(std::string_view text, SegmentOk segmentOk) noexcept
{
    std::size_t count = 0;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t dot = text.find('.', start);
        const std::string_view segment =
            text.substr(start, (dot == std::string_view::npos ? text.size() : dot) - start);
        if (segment.empty() || !segmentOk(segment))
        {
            return 0;
        }
        ++count;
        if (dot == std::string_view::npos)
        {
            return count;
        }
        start = dot + 1;
    }
}

} // namespace

bool isValidPackageId(std::string_view id) noexcept
{
    if (id.size() > 128)
    {
        return false;
    }
    const auto segmentOk = [](std::string_view segment) {
        if (segment.front() == '-' || segment.back() == '-')
        {
            return false;
        }
        for (const char c : segment)
        {
            if (!isLowerAlpha(c) && !isDigit(c) && c != '-')
            {
                return false;
            }
        }
        return true;
    };
    return checkSegments(id, segmentOk) >= 2;
}

bool isValidBuiltinName(std::string_view name) noexcept
{
    if (name.size() > 64)
    {
        return false;
    }
    const auto segmentOk = [](std::string_view segment) {
        if (!isLowerAlpha(segment.front()) || segment.back() == '-')
        {
            return false;
        }
        for (const char c : segment)
        {
            if (!isLowerAlpha(c) && !isDigit(c) && c != '-')
            {
                return false;
            }
        }
        return true;
    };
    return checkSegments(name, segmentOk) >= 1;
}

bool isValidPaletteName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 32 || !isLowerAlpha(name.front()))
    {
        return false;
    }
    for (const char c : name)
    {
        if (!isLowerAlpha(c) && !isDigit(c) && c != '-' && c != '_')
        {
            return false;
        }
    }
    return true;
}

} // namespace rmw::domain
