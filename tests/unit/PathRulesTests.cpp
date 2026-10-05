#include "rmw/domain/PathRules.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using rmw::domain::hasExtension;
using rmw::domain::validatePackagePath;

TEST(PathRules, AcceptsAndNormalizesRelativePaths)
{
    EXPECT_EQ(validatePackagePath("assets/city.mp4").value(), "assets/city.mp4");
    EXPECT_EQ(validatePackagePath("assets\\cursors\\pointer.cur").value(), "assets/cursors/pointer.cur");
    EXPECT_EQ(validatePackagePath("Índice/夜景 🎴.png").value(), "Índice/夜景 🎴.png");
    EXPECT_EQ(validatePackagePath("a/.hidden").value(), "a/.hidden");
    EXPECT_EQ(validatePackagePath("console.png").value(), "console.png"); // starts with "con" but is not CON
    EXPECT_EQ(validatePackagePath("com10.txt").value(), "com10.txt");
    EXPECT_EQ(validatePackagePath("a~b.png").value(), "a~b.png");
}

struct RejectCase
{
    const char* name;
    std::string path;
};

class PathRulesRejects : public testing::TestWithParam<RejectCase>
{
};

TEST_P(PathRulesRejects, Rejects)
{
    const auto result = validatePackagePath(GetParam().path);
    EXPECT_FALSE(result.has_value()) << GetParam().path;
}

INSTANTIATE_TEST_SUITE_P(
    PathRules, PathRulesRejects,
    testing::Values(RejectCase{"Empty", ""}, RejectCase{"Absolute", "/etc/x"},
                    RejectCase{"AbsoluteBackslash", "\\x"}, RejectCase{"DriveLetter", "C:/Windows/x.png"},
                    RejectCase{"DriveRelative", "C:x.png"}, RejectCase{"Unc", "\\\\server\\share\\x"},
                    RejectCase{"DevicePrefix", "\\\\?\\C:\\x"},
                    RejectCase{"DotDevicePrefix", "\\\\.\\pipe\\x"}, RejectCase{"DotDot", "assets/../../x"},
                    RejectCase{"Dot", "./x"}, RejectCase{"EmptyComponent", "a//b"},
                    RejectCase{"TrailingSlash", "a/"}, RejectCase{"AlternateStream", "a.png:evil"},
                    RejectCase{"Wildcard", "a*.png"}, RejectCase{"Question", "a?.png"},
                    RejectCase{"Pipe", "a|b"}, RejectCase{"Quote", "a\"b"}, RejectCase{"Less", "a<b"},
                    RejectCase{"Control", std::string{'a', '\x01', 'b'}},
                    RejectCase{"Nul", std::string("a\0b", 3)}, RejectCase{"TrailingDot", "assets./x"},
                    RejectCase{"TrailingSpace", "x.png "}, RejectCase{"Con", "con"},
                    RejectCase{"ConWithExtension", "CON.png"}, RejectCase{"NulInFolder", "a/nul.txt/b"},
                    RejectCase{"Com1", "com1.cur"}, RejectCase{"Lpt9", "LPT9"}, RejectCase{"Com0", "com0"},
                    RejectCase{"ComSuperscript", "COM\xC2\xB9.txt"},
                    RejectCase{"LptSuperscript", "lpt\xC2\xB3"}, RejectCase{"ShortName", "PROGRA~1/x"},
                    RejectCase{"InvalidUtf8", "a\xC3"}, RejectCase{"Overlong", std::string(161, 'a')},
                    RejectCase{"LongComponent", std::string(100, 'a') + "/" + std::string(256, 'b')}),
    [](const testing::TestParamInfo<RejectCase>& info) { return std::string(info.param.name); });

TEST(PathRules, ChecksExtensionsCaseInsensitively)
{
    EXPECT_TRUE(hasExtension("a/b.CUR", {"cur", "ani"}));
    EXPECT_TRUE(hasExtension("b.ani", {"cur", "ani"}));
    EXPECT_FALSE(hasExtension("b.png", {"cur", "ani"}));
    EXPECT_FALSE(hasExtension("cur", {"cur"}));
    EXPECT_FALSE(hasExtension("a.cur/b", {"cur"}));
    EXPECT_FALSE(hasExtension("b.cur.exe", {"cur"}));
}

} // namespace
