#include "rmw/base/Error.hpp"

#include <gtest/gtest.h>

namespace
{

rmw::Result<int> parsePositive(int value)
{
    if (value <= 0)
    {
        return rmw::fail(rmw::ErrorCode::InvalidArgument, "value must be positive");
    }
    return value;
}

TEST(Error, CarriesCodeMessageAndNativeCode)
{
    const rmw::Error error(rmw::ErrorCode::System, "boom", 5);
    EXPECT_EQ(error.code(), rmw::ErrorCode::System);
    EXPECT_EQ(error.message(), "boom");
    EXPECT_EQ(error.nativeCode(), 5);
}

TEST(Error, WithContextPrependsText)
{
    rmw::Error error(rmw::ErrorCode::NotFound, "file not found");
    error.withContext("loading rice");
    EXPECT_EQ(error.message(), "loading rice: file not found");
}

TEST(Error, ToStringContainsCategoryFileAndNativeCode)
{
    const rmw::Error error(rmw::ErrorCode::System, "denied", 0x5);
    const std::string text = error.toString();
    EXPECT_NE(text.find("System: denied"), std::string::npos);
    EXPECT_NE(text.find("0x00000005"), std::string::npos);
    EXPECT_NE(text.find("ErrorTests.cpp"), std::string::npos);
}

TEST(Error, ResultPropagatesValuesAndErrors)
{
    EXPECT_EQ(parsePositive(3).value(), 3);
    const auto bad = parsePositive(-1);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), rmw::ErrorCode::InvalidArgument);
}

TEST(Error, EveryCodeHasAName)
{
    for (auto code = static_cast<int>(rmw::ErrorCode::Unknown);
         code <= static_cast<int>(rmw::ErrorCode::Database); ++code)
    {
        EXPECT_NE(rmw::toString(static_cast<rmw::ErrorCode>(code)), "");
    }
}

} // namespace
