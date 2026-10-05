#include "CursorSamples.hpp"

#include "rmw/domain/CursorFiles.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{

using namespace rmw::domain;

using namespace rmw::test;

TEST(CursorFiles, AcceptsWellFormedCursors)
{
    EXPECT_EQ(validateCursorFile(cursorFile(), "cur").value(), CursorFormat::Static);
    EXPECT_EQ(validateCursorFile(cursorFile(), "CUR").value(), CursorFormat::Static);
    EXPECT_EQ(validateCursorFile(aniFile(), "ani").value(), CursorFormat::Animated);
    EXPECT_EQ(validateCursorFile(aniFile({.sequence = {1, 0}}), "ani").value(), CursorFormat::Animated);

    Writer png;
    png.u8(0x89).tag("PNG").u8(0x0D).u8(0x0A).u8(0x1A).u8(0x0A).u32(0x0D000000).tag("IHDR");
    png.u8(0).u8(0).u8(0).u8(48).u8(0).u8(0).u8(0).u8(48).zeros(5 + 4); // rest of IHDR + CRC
    EXPECT_TRUE(validateCursorFile(cursorFile(2, png.bytes), "cur").has_value());
}

TEST(CursorFiles, RejectsMalformedStaticCursors)
{
    auto truncated = cursorFile();
    truncated.resize(truncated.size() - 10);
    const std::vector<std::vector<std::byte>> bad = {
        {},
        cursorFile(1),                                // an icon, not a cursor
        cursorFile(2, monochromeImage(), 2),          // image overlaps the directory
        cursorFile(2, monochromeImage(), 0xFFFFFFF0), // image outside the file
        truncated,
        cursorFile(2, monochromeImage(0)),         // zero width
        cursorFile(2, monochromeImage(32, 64, 3)), // 3 bits per pixel
        cursorFile(2, monochromeImage(1000)),      // too wide
    };
    for (std::size_t i = 0; i < bad.size(); ++i)
    {
        EXPECT_FALSE(validateCursorFile(bad[i], "cur").has_value()) << "case " << i;
    }
    EXPECT_FALSE(validateCursorFile(cursorFile(), "ani").has_value()); // extension must match
    EXPECT_FALSE(validateCursorFile(cursorFile(), "png").has_value());
}

TEST(CursorFiles, RejectsMalformedAnimatedCursors)
{
    EXPECT_FALSE(validateCursorFile(aniFile({.storedFrames = 1}), "ani").has_value()); // header says 2
    EXPECT_FALSE(validateCursorFile(aniFile({.frames = 0, .storedFrames = 0}), "ani").has_value());
    EXPECT_FALSE(validateCursorFile(aniFile({.flags = 0}), "ani").has_value());         // raw bitmaps
    EXPECT_FALSE(validateCursorFile(aniFile({.sequence = {0, 5}}), "ani").has_value()); // frame 5 missing
    EXPECT_FALSE(validateCursorFile(aniFile({.sequence = {0}}), "ani").has_value());    // wrong size

    auto lying = aniFile();
    lying[4] = std::byte{0xFF}; // RIFF size beyond the file
    lying[5] = std::byte{0xFF};
    EXPECT_FALSE(validateCursorFile(lying, "ani").has_value());
    auto cut = aniFile();
    cut.resize(cut.size() / 2);
    EXPECT_FALSE(validateCursorFile(cut, "ani").has_value());
}

TEST(CursorFiles, NeverReadsOutOfBoundsOnTruncations)
{
    // Every prefix of valid files must be rejected or accepted without crashing (ASan watches).
    for (const auto& file : {cursorFile(), aniFile({.sequence = {1, 0}})})
    {
        for (std::size_t length = 0; length < file.size(); ++length)
        {
            [[maybe_unused]] const auto asCur = validateCursorFile(std::span(file).first(length), "cur");
            [[maybe_unused]] const auto asAni = validateCursorFile(std::span(file).first(length), "ani");
        }
    }
}

constexpr std::string_view kSchemeInf = R"inf(; Cursor pack
[Version]
signature="$CHICAGO$"

[DefaultInstall]
CopyFiles = Scheme.Cur
AddReg    = Scheme.Reg

[Scheme.Reg]
HKCU,"Control Panel\Cursors\Schemes","%SCHEME_NAME%",,"%10%\%CUR_DIR%\%pointer%,%10%\%CUR_DIR%\%help%,%10%\%CUR_DIR%\%work%,,,%10%\%CUR_DIR%\%text%"

[Strings]
CUR_DIR     = "Cursors\Neon"
SCHEME_NAME = "Neon"
pointer     = "Normal Select.cur"
help        = "help.cur"
work        = "working.ani"
text        = "beam.cur"
)inf";

TEST(CursorInf, ReadsTheSchemeLine)
{
    const auto assignments = parseCursorInf(kSchemeInf);
    ASSERT_TRUE(assignments.has_value()) << assignments.error().toString();
    ASSERT_EQ(assignments->size(), 4u);
    EXPECT_EQ((*assignments)[0].role, CursorRole::Normal);
    EXPECT_EQ((*assignments)[0].file, "Normal Select.cur");
    EXPECT_EQ((*assignments)[2].role, CursorRole::Working);
    EXPECT_EQ((*assignments)[2].file, "working.ani");
    EXPECT_EQ((*assignments)[3].role, CursorRole::Text); // empty entries keep the positions
}

TEST(CursorInf, ReadsOneValuePerRole)
{
    const auto assignments =
        parseCursorInf("[DefaultInstall]\nAddReg=R\n[R]\n"
                       "HKCU,\"Control Panel\\Cursors\",\"Arrow\",0x00020000,\"%10%\\x\\a.cur\"\n"
                       "HKCU,\"Control Panel\\Cursors\",\"Hand\",0x00020000,\"%10%\\x\\link.ani\"\n");
    ASSERT_TRUE(assignments.has_value()) << assignments.error().toString();
    ASSERT_EQ(assignments->size(), 2u);
    EXPECT_EQ((*assignments)[1].role, CursorRole::Link);
}

TEST(CursorInf, RejectsHostileOrUselessFiles)
{
    for (const char* inf : {
             "[Strings]\nx=1\n",                                                        // no scheme
             "[R]\nHKCU,\"Control Panel\\Cursors\\Schemes\",\"N\",,\"CON.cur\"\n",      // device name
             "[R]\nHKCU,\"Control Panel\\Cursors\\Schemes\",\"N\",,\"evil.exe\"\n",     // not a cursor
             "[R]\nHKCU,\"Control Panel\\Cursors\\Schemes\",\"N\",,\"a.cur:stream\"\n", // ADS
         })
    {
        EXPECT_FALSE(parseCursorInf(inf).has_value()) << inf;
    }
    // Paths are reduced to the file name: "..\..\x.cur" becomes "x.cur" inside the cursor folder.
    const auto traversal =
        parseCursorInf("[R]\nHKCU,\"Control Panel\\Cursors\\Schemes\",\"N\",,\"..\\..\\x.cur\"\n");
    ASSERT_TRUE(traversal.has_value());
    EXPECT_EQ((*traversal)[0].file, "x.cur");
    EXPECT_FALSE(parseCursorInf(std::string(70'000, 'x')).has_value());
}

} // namespace
