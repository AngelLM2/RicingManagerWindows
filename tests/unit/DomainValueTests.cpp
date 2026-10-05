// Small value types of rmw::domain: Color, SemVer/VersionReq, identifiers, TimeOfDay, cursor roles.

#include "rmw/domain/Color.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/domain/SemVer.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace
{

using namespace rmw::domain;

TEST(Color, ParsesShortLongAndAlphaForms)
{
    EXPECT_EQ(Color::parse("#fff").value(), (Color{255, 255, 255, 255}));
    EXPECT_EQ(Color::parse("#7aa2f7").value(), (Color{0x7a, 0xa2, 0xf7, 255}));
    EXPECT_EQ(Color::parse("#7AA2F780").value(), (Color{0x7a, 0xa2, 0xf7, 0x80}));
}

TEST(Color, RejectsMalformedText)
{
    for (const char* text :
         {"", "#", "fff", "#ff", "#ffff", "#fffff", "#fffffff", "#gggggg", "# ffffff", "#ffffff "})
    {
        EXPECT_FALSE(Color::parse(text).has_value()) << text;
    }
}

TEST(Color, FormatsAsUpperHex)
{
    EXPECT_EQ((Color{0x1a, 0x1b, 0x26, 255}).toHex(), "#1A1B26");
    EXPECT_EQ((Color{0x1a, 0x1b, 0x26, 0x40}).toHex(), "#1A1B2640");
}

TEST(SemVer, ParsesAndCompares)
{
    const auto version = SemVer::parse("1.20.3-beta.1");
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(version->major, 1);
    EXPECT_EQ(version->minor, 20);
    EXPECT_EQ(version->patch, 3);
    EXPECT_EQ(version->prerelease, "beta.1");
    EXPECT_EQ(version->toString(), "1.20.3-beta.1");
    EXPECT_LT(SemVer::parse("0.9.9").value(), SemVer::parse("0.10.0").value());
    EXPECT_EQ(SemVer::parse("1.0.0").value(), SemVer::parse("1.0.0-rc").value()); // prerelease ignored
}

TEST(SemVer, RejectsMalformedVersions)
{
    for (const char* text : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "1..3", "1.2.3-", "-1.2.3",
                             "1.2.3 ", "9999999.0.0", "+1.2.3"})
    {
        EXPECT_FALSE(SemVer::parse(text).has_value()) << text;
    }
}

struct ReqCase
{
    const char* req;
    const char* version;
    bool matches;
};

TEST(VersionReq, FollowsCargoSemantics)
{
    const ReqCase cases[] = {
        {"^1.2", "1.2.0", true},    {"^1.2", "1.9.9", true},    {"^1.2", "2.0.0", false},
        {"^1.2", "1.1.9", false},   {"1.2", "1.5.0", true},     {"^0.2.3", "0.2.9", true},
        {"^0.2.3", "0.3.0", false}, {"^0.0.3", "0.0.3", true},  {"^0.0.3", "0.0.4", false},
        {"^0", "0.9.0", true},      {"^0", "1.0.0", false},     {"~1.2.3", "1.2.9", true},
        {"~1.2.3", "1.3.0", false}, {"~1", "1.9.0", true},      {"~1", "2.0.0", false},
        {"=1.2.3", "1.2.3", true},  {"=1.2.3", "1.2.4", false}, {">=1.0", "7.0.0", true},
        {">=1.0", "0.9.0", false},  {"*", "0.0.1", true},
    };
    for (const auto& c : cases)
    {
        const auto req = VersionReq::parse(c.req);
        ASSERT_TRUE(req.has_value()) << c.req;
        EXPECT_EQ(req->matches(SemVer::parse(c.version).value()), c.matches) << c.req << " vs " << c.version;
    }
}

TEST(VersionReq, RejectsMalformedRequirements)
{
    for (const char* text : {"", "^", "^x", "=1.2", "^1.2.3.4", ">1.0", "1.2.3-", "^01"})
    {
        EXPECT_FALSE(VersionReq::parse(text).has_value()) << text;
    }
}

TEST(Identifiers, PackageIds)
{
    EXPECT_TRUE(isValidPackageId("io.github.fulano.now-playing"));
    EXPECT_TRUE(isValidPackageId("com.example2.x"));
    for (const char* id :
         {"", "single", "Io.github.x", "io..x", ".io.x", "io.x.", "io.-x", "io.x-", "io.x_y", "io.x y"})
    {
        EXPECT_FALSE(isValidPackageId(id)) << id;
    }
    EXPECT_FALSE(isValidPackageId(std::string(129, 'a') + ".b"));
}

TEST(Identifiers, BuiltinAndPaletteNames)
{
    EXPECT_TRUE(isValidBuiltinName("clock"));
    EXPECT_TRUE(isValidBuiltinName("media.control"));
    EXPECT_FALSE(isValidBuiltinName("1clock"));
    EXPECT_FALSE(isValidBuiltinName("Clock"));
    EXPECT_FALSE(isValidBuiltinName("clock-"));
    EXPECT_TRUE(isValidPaletteName("base"));
    EXPECT_TRUE(isValidPaletteName("surface_2"));
    EXPECT_FALSE(isValidPaletteName("2nd"));
    EXPECT_FALSE(isValidPaletteName("a.b"));
}

TEST(TimeOfDay, ParsesTwentyFourHourClock)
{
    EXPECT_EQ(TimeOfDay::parse("07:05").value(), (TimeOfDay{7, 5}));
    EXPECT_EQ(TimeOfDay::parse("23:59").value(), (TimeOfDay{23, 59}));
    for (const char* text : {"24:00", "7:00", "07:60", "07-00", "", "07:00:00"})
    {
        EXPECT_FALSE(TimeOfDay::parse(text).has_value()) << text;
    }
}

// Audit finding C3: "help", "pin" and "person" once overwrote the normal arrow. Every role must
// map to its own registry value.
TEST(CursorRoles, MapToDistinctRegistryValues)
{
    std::set<std::string_view> riceNames;
    std::set<std::string_view> registryNames;
    for (std::size_t i = 0; i < kCursorRoleCount; ++i)
    {
        const auto role = static_cast<CursorRole>(i);
        riceNames.insert(riceName(role));
        registryNames.insert(registryValueName(role));
        EXPECT_EQ(cursorRoleFromRiceName(riceName(role)), role);
    }
    EXPECT_EQ(riceNames.size(), kCursorRoleCount);
    EXPECT_EQ(registryNames.size(), kCursorRoleCount);
    EXPECT_EQ(registryValueName(CursorRole::Normal), "Arrow");
    EXPECT_EQ(registryValueName(CursorRole::Help), "Help");
    EXPECT_EQ(registryValueName(CursorRole::Location), "Pin");
    EXPECT_EQ(registryValueName(CursorRole::Person), "Person");
    EXPECT_EQ(registryValueName(CursorRole::Link), "Hand");
    EXPECT_FALSE(cursorRoleFromRiceName("arrow").has_value());
}

} // namespace
