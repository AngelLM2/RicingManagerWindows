#include "rmw/domain/SemVer.hpp"

#include <array>
#include <charconv>
#include <format>

namespace rmw::domain
{
namespace
{

constexpr std::size_t kMaxDigits = 6;

// Parses "1", "1.2" or "1.2.3" into `out`; returns how many components were present (0 = error).
int parseComponents(std::string_view text, SemVer& out, int minComponents)
{
    const std::array<int*, 3> parts = {&out.major, &out.minor, &out.patch};
    int count = 0;
    std::size_t start = 0;
    while (true)
    {
        if (count == 3)
        {
            return 0; // more than three components
        }
        const std::size_t dot = text.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? text.size() : dot;
        const std::string_view number = text.substr(start, end - start);
        if (number.empty() || number.size() > kMaxDigits || (number.size() > 1 && number.front() == '0'))
        {
            return 0;
        }
        int* const target = parts.at(static_cast<std::size_t>(count));
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), *target);
        if (ec != std::errc{} || ptr != number.data() + number.size())
        {
            return 0;
        }
        ++count;
        if (dot == std::string_view::npos)
        {
            break;
        }
        start = dot + 1;
    }
    return count >= minComponents ? count : 0;
}

} // namespace

Result<SemVer> SemVer::parse(std::string_view text)
{
    SemVer version;
    std::string_view core = text;
    if (const auto dash = text.find('-'); dash != std::string_view::npos)
    {
        core = text.substr(0, dash);
        version.prerelease = std::string(text.substr(dash + 1));
    }
    if ((core.size() != text.size() && version.prerelease.empty()) || parseComponents(core, version, 3) == 0)
    {
        return fail(ErrorCode::InvalidData,
                    std::format("invalid version '{}' (expected MAJOR.MINOR.PATCH)", text));
    }
    return version;
}

std::string SemVer::toString() const
{
    if (prerelease.empty())
    {
        return std::format("{}.{}.{}", major, minor, patch);
    }
    return std::format("{}.{}.{}-{}", major, minor, patch, prerelease);
}

Result<VersionReq> VersionReq::parse(std::string_view text)
{
    VersionReq req;
    std::string_view rest = text;
    if (rest == "*")
    {
        return req;
    }
    if (rest.starts_with(">="))
    {
        req.m_op = Op::AtLeast;
        rest.remove_prefix(2);
    }
    else if (rest.starts_with('^'))
    {
        req.m_op = Op::Caret;
        rest.remove_prefix(1);
    }
    else if (rest.starts_with('~'))
    {
        req.m_op = Op::Tilde;
        rest.remove_prefix(1);
    }
    else if (rest.starts_with('='))
    {
        req.m_op = Op::Exact;
        rest.remove_prefix(1);
    }
    else
    {
        req.m_op = Op::Caret;
    }

    req.m_components = parseComponents(rest, req.m_base, req.m_op == Op::Exact ? 3 : 1);
    if (req.m_components == 0)
    {
        return fail(
            ErrorCode::InvalidData,
            std::format("invalid version requirement '{}' (examples: ^1.2, ~1.2.3, =1.0.0, >=1.0, *)", text));
    }
    return req;
}

bool VersionReq::matches(const SemVer& version) const noexcept
{
    if (m_op == Op::Any)
    {
        return true;
    }
    if (version < m_base)
    {
        return false;
    }
    switch (m_op)
    {
    case Op::Exact:
        return version == m_base;
    case Op::AtLeast:
        return true;
    case Op::Tilde:
        // ~1 -> <2.0.0; ~1.2 and ~1.2.3 -> <1.3.0
        if (m_components == 1)
        {
            return version.major == m_base.major;
        }
        return version.major == m_base.major && version.minor == m_base.minor;
    case Op::Caret:
        // The first non-zero written component may not change.
        if (m_base.major != 0 || m_components == 1)
        {
            return version.major == m_base.major;
        }
        if (m_base.minor != 0 || m_components == 2)
        {
            return version.major == 0 && version.minor == m_base.minor;
        }
        return version.major == 0 && version.minor == 0 && version.patch == m_base.patch;
    case Op::Any:
        break;
    }
    return true;
}

std::string VersionReq::toString() const
{
    std::string base = std::to_string(m_base.major);
    if (m_components >= 2)
    {
        base += std::format(".{}", m_base.minor);
    }
    if (m_components >= 3)
    {
        base += std::format(".{}", m_base.patch);
    }
    switch (m_op)
    {
    case Op::Any:
        return "*";
    case Op::Caret:
        return "^" + base;
    case Op::Tilde:
        return "~" + base;
    case Op::Exact:
        return "=" + base;
    case Op::AtLeast:
        return ">=" + base;
    }
    return base;
}

} // namespace rmw::domain
