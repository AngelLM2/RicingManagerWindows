#include "rmw/base/Utf.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(Utf, RoundTripsAsciiAccentsCjkAndEmoji)
{
    const std::string original = "wallpaper Índice 夜景 🎴.png";
    const auto wide = rmw::utf8To16(original);
    ASSERT_TRUE(wide.has_value()) << wide.error().toString();
    const auto back = rmw::utf16To8(*wide);
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    EXPECT_EQ(*back, original);
}

TEST(Utf, EncodesSupplementaryPlaneAsSurrogatePair)
{
    const auto wide = rmw::utf8To16("\xF0\x9F\x8E\xB4"); // U+1F3B4
    ASSERT_TRUE(wide.has_value());
    ASSERT_EQ(wide->size(), 2u);
    EXPECT_EQ((*wide)[0], static_cast<wchar_t>(0xD83C));
    EXPECT_EQ((*wide)[1], static_cast<wchar_t>(0xDFB4));
}

TEST(Utf, EmptyInputIsValid)
{
    EXPECT_TRUE(rmw::utf8To16("").has_value());
    EXPECT_TRUE(rmw::utf16To8(L"").has_value());
}

struct InvalidUtf8Case
{
    const char* name;
    std::string bytes;
};

class InvalidUtf8 : public ::testing::TestWithParam<InvalidUtf8Case>
{
};

TEST_P(InvalidUtf8, IsRejectedStrictlyAndReplacedLossily)
{
    const auto& input = GetParam().bytes;
    const auto strict = rmw::utf8To16(input);
    ASSERT_FALSE(strict.has_value());
    EXPECT_EQ(strict.error().code(), rmw::ErrorCode::InvalidData);
    EXPECT_FALSE(rmw::isValidUtf8(input));
    EXPECT_NE(rmw::utf8To16Lossy(input).find(L'\xFFFD'), std::wstring::npos);
}

INSTANTIATE_TEST_SUITE_P(Utf, InvalidUtf8,
                         ::testing::Values(InvalidUtf8Case{"LoneContinuation", "\x80"},
                                           InvalidUtf8Case{"InvalidLead", "\xFF"},
                                           InvalidUtf8Case{"OverlongSlash", "\xC0\xAF"},
                                           InvalidUtf8Case{"OverlongNul", "\xE0\x80\x80"},
                                           InvalidUtf8Case{"EncodedSurrogate", "\xED\xA0\x80"},
                                           InvalidUtf8Case{"AboveMaxCodePoint", "\xF4\x90\x80\x80"},
                                           InvalidUtf8Case{"TruncatedTwoByte", "abc\xC3"},
                                           InvalidUtf8Case{"TruncatedFourByte", "\xF0\x9F\x8E"},
                                           InvalidUtf8Case{"BadContinuation", "\xE2\x28\xA1"}),
                         [](const ::testing::TestParamInfo<InvalidUtf8Case>& info) {
                             return std::string(info.param.name);
                         });

TEST(Utf, ErrorReportsTheOffendingByte)
{
    const auto result = rmw::utf8To16("abc\xFF");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("byte 3"), std::string::npos);
}

TEST(Utf, RejectsUnpairedSurrogatesInUtf16)
{
    const std::wstring loneHigh(1, static_cast<wchar_t>(0xD800));
    const std::wstring loneLow(1, static_cast<wchar_t>(0xDC00));
    EXPECT_FALSE(rmw::utf16To8(loneHigh).has_value());
    EXPECT_FALSE(rmw::utf16To8(loneLow).has_value());
    EXPECT_EQ(rmw::utf16To8Lossy(loneHigh), "\xEF\xBF\xBD");
}

TEST(Utf, LossyKeepsValidTextAroundErrors)
{
    EXPECT_EQ(rmw::utf8To16Lossy("a\xFF"
                                 "b"),
              L"a\xFFFD"
              L"b");
}

TEST(Utf, APrefixNeverCutsACharacter)
{
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC"; // two characters, three bytes each
    EXPECT_EQ(rmw::utf8Prefix(japanese, 6), japanese);
    EXPECT_EQ(rmw::utf8Prefix(japanese, 5), "\xE6\x97\xA5");
    EXPECT_EQ(rmw::utf8Prefix(japanese, 4), "\xE6\x97\xA5");
    EXPECT_EQ(rmw::utf8Prefix(japanese, 2), "");
    EXPECT_EQ(rmw::utf8Prefix("ab\xC3\xA7", 3), "ab"); // ç
    EXPECT_EQ(rmw::utf8Prefix("abc", 0), "");
    for (std::size_t n = 0; n <= japanese.size(); ++n)
    {
        EXPECT_TRUE(rmw::isValidUtf8(rmw::utf8Prefix(japanese, n))) << n;
    }
}

} // namespace
