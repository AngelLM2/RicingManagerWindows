#include "rmw/base/CheckedCast.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace
{

TEST(CheckedCast, AcceptsValuesInRange)
{
    EXPECT_EQ(rmw::checkedCast<std::uint32_t>(std::size_t{42}), 42u);
    EXPECT_EQ(rmw::checkedCast<int>(std::uint8_t{255}), 255);
    EXPECT_EQ(rmw::narrowOrFail<std::uint16_t>(65535), 65535);
}

TEST(CheckedCast, RejectsNegativeToUnsigned)
{
    EXPECT_FALSE(rmw::checkedCast<std::uint32_t>(-1).has_value());
}

TEST(CheckedCast, RejectsOverflow)
{
    EXPECT_FALSE(rmw::checkedCast<std::uint32_t>(std::uint64_t{1} << 32).has_value());
    EXPECT_FALSE(rmw::checkedCast<int>(std::numeric_limits<std::size_t>::max()).has_value());
}

// narrowOrFail's failure path (__fastfail) is not exercised here: a fast fail goes straight to
// Windows Error Reporting and could open a dialog during local runs.

} // namespace
