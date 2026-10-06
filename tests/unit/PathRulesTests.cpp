#include "rmw/domain/PathRules.hpp"

#include "rmw/base/Utf.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using rmw::domain::hasExtension;
using rmw::domain::safeFileName;
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

TEST(PathRules, SafeNamesKeepTheirExtensionWhenCut)
{
    EXPECT_EQ(safeFileName("city night.png", "x"), "city night.png");
    EXPECT_EQ(safeFileName("a<b>:c.png", "x"), "a-b--c.png");
    EXPECT_EQ(safeFileName("con.png", "fallback.png"), "fallback.png");

    // A long name loses the end of its stem; the rice that names it still reads (v1.7: "<long>" with no .jpg).
    const std::string longName = std::string(80, 'w') + ".jpg";
    const std::string cut = safeFileName(longName, "x");
    EXPECT_EQ(cut.size(), 60u);
    EXPECT_TRUE(cut.ends_with(".jpg"));
    EXPECT_TRUE(hasExtension(cut, {"jpg"}));

    // Never half a character: 3-byte characters across the cut.
    std::string wide;
    for (int i = 0; i < 30; ++i)
    {
        wide += "\xE5\xA4\x9C"; // 夜
    }
    const std::string cutWide = safeFileName(wide + ".webm", "x");
    EXPECT_LE(cutWide.size(), 60u);
    EXPECT_TRUE(cutWide.ends_with(".webm"));
    EXPECT_TRUE(rmw::isValidUtf8(cutWide));
    EXPECT_TRUE(validatePackagePath(cutWide));

    // No extension worth keeping (a dot in a sentence): cut like any text, no trailing dot or space.
    const std::string sentence = std::string(58, 'a') + " . b";
    const std::string cutSentence = safeFileName(sentence + std::string(20, 'c'), "x");
    EXPECT_LE(cutSentence.size(), 60u);
    EXPECT_FALSE(cutSentence.ends_with('.'));
    EXPECT_FALSE(cutSentence.ends_with(' '));
}

} // namespace
