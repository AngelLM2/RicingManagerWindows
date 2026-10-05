#pragma once

#include "rmw/base/Error.hpp"

#include <compare>
#include <string>
#include <string_view>

namespace rmw::domain
{

// "MAJOR.MINOR.PATCH" with an optional "-prerelease" suffix (kept, but ignored when comparing).
struct SemVer
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;

    static Result<SemVer> parse(std::string_view text);
    std::string toString() const;

    friend bool operator==(const SemVer& a, const SemVer& b) noexcept
    {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    friend std::strong_ordering operator<=>(const SemVer& a, const SemVer& b) noexcept
    {
        if (auto c = a.major <=> b.major; c != 0)
        {
            return c;
        }
        if (auto c = a.minor <=> b.minor; c != 0)
        {
            return c;
        }
        return a.patch <=> b.patch;
    }
};

// Version requirement with Cargo semantics: "^1.2" (also the bare "1.2"), "~1.2.3", "=1.2.3",
// ">=1.0", "*". Missing components count as 0 for the lower bound.
class VersionReq
{
public:
    enum class Op
    {
        Any,
        Caret,
        Tilde,
        Exact,
        AtLeast,
    };

    static Result<VersionReq> parse(std::string_view text);

    bool matches(const SemVer& version) const noexcept;
    std::string toString() const;

    Op op() const noexcept { return m_op; }

private:
    Op m_op = Op::Any;
    SemVer m_base;
    int m_components = 0; // how many of major/minor/patch were written
};

} // namespace rmw::domain
