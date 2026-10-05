#include "rmw/json/Json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using rmw::ErrorCode;
using namespace rmw::json;

TEST(Json, ParsesAndDumps)
{
    const auto value = parse(R"({"a": [1, 2.5, "x", true, null], "b": {"c": "夜景"}})");
    ASSERT_TRUE(value.has_value()) << value.error().toString();
    EXPECT_EQ(dump(*value), R"({"a":[1,2.5,"x",true,null],"b":{"c":"夜景"}})");
}

TEST(Json, EnforcesLimitsBeforeParsing)
{
    EXPECT_EQ(parse(std::string(40, '[') + std::string(40, ']')).error().code(), ErrorCode::LimitExceeded);
    // Brackets inside strings do not count.
    EXPECT_TRUE(parse("\"" + std::string(100, '[') + "\"").has_value());
    // Escaped quotes do not end the string early.
    EXPECT_TRUE(parse(R"(["\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["])").has_value());

    ParseLimits small;
    small.maxBytes = 8;
    EXPECT_EQ(parse("[1,2,3,4,5]", small).error().code(), ErrorCode::LimitExceeded);
    small = {};
    small.maxElements = 3;
    EXPECT_EQ(parse("[1,2,3,4,5]", small).error().code(), ErrorCode::LimitExceeded);
}

TEST(Json, RejectsMalformedInput)
{
    for (const char* text : {"", "{", "[1,]", "{\"a\" 1}", "nul", "\"\xC3\x28\"", "[1] [2]"})
    {
        const auto value = parse(text);
        ASSERT_FALSE(value.has_value()) << text;
        EXPECT_EQ(value.error().code(), ErrorCode::InvalidData) << text;
    }
}

TEST(Json, DumpNeverThrowsOnInvalidUtf8)
{
    const Json value = std::string("a\xFF"
                                   "b");
    EXPECT_NO_THROW({ (void)dump(value); });
}

TEST(JsonReader, ReadsTypedMembers)
{
    const Json value = parse(R"({"s":"x","i":-5,"u":7,"d":1.5,"b":false,"o":{"k":1},"a":[1]})").value();
    const Reader reader(value, "config");
    EXPECT_EQ(reader.string("s").value(), "x");
    EXPECT_EQ(reader.integer("i").value(), -5);
    EXPECT_EQ(reader.integer("u").value(), 7);
    EXPECT_DOUBLE_EQ(reader.number("d").value(), 1.5);
    EXPECT_DOUBLE_EQ(reader.number("i").value(), -5.0);
    EXPECT_FALSE(reader.boolean("b").value());
    EXPECT_EQ(reader.object("o")->integer("k").value(), 1);
    EXPECT_EQ(reader.array("a").value()->size(), 1u);
    EXPECT_FALSE(reader.optionalString("missing").value().has_value());
    EXPECT_EQ(reader.optionalInteger("i").value(), -5);
}

TEST(JsonReader, ErrorsNameThePath)
{
    const Json value = parse(R"({"o":{"k":"text"},"big":18446744073709551615})").value();
    const Reader reader(value, "config");
    const auto wrongType = reader.object("o")->integer("k");
    ASSERT_FALSE(wrongType.has_value());
    EXPECT_NE(wrongType.error().message().find("config.o.k"), std::string::npos)
        << wrongType.error().message();
    EXPECT_NE(wrongType.error().message().find("found string"), std::string::npos);

    EXPECT_NE(reader.string("nope").error().message().find("missing 'config.nope'"), std::string::npos);
    EXPECT_FALSE(reader.integer("big").has_value()); // above int64
    EXPECT_FALSE(reader.optionalBoolean("o").has_value());

    const Json notObject = Json::array();
    EXPECT_FALSE(Reader(notObject, "").string("x").has_value());
}

} // namespace
