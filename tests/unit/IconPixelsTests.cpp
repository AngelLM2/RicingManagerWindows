#include "rmw/providers/Windows.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace
{

using rmw::providers::IconPixels;
using rmw::providers::shrinkPixels;

IconPixels solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    IconPixels out;
    out.width = width;
    out.height = height;
    for (int i = 0; i < width * height; ++i)
    {
        out.rgba.insert(out.rgba.end(), {r, g, b, a});
    }
    return out;
}

TEST(IconPixels, TheShellsLargePicturesAreShrunkToTheSizeShown)
{
    // 256 px from the Shell, 48 asked: 9 KB kept instead of 256 KB (239 apps passed the bar's limit).
    const IconPixels big = solid(256, 256, 10, 200, 30, 255);
    const IconPixels small = shrinkPixels(big, 48);
    EXPECT_EQ(small.width, 48);
    EXPECT_EQ(small.height, 48);
    EXPECT_EQ(small.rgba.size(), 48u * 48u * 4u);
    EXPECT_EQ(small.rgba[0], 10);
    EXPECT_EQ(small.rgba[1], 200);
    EXPECT_EQ(small.rgba[3], 255);
    // Already small enough: as it is.
    EXPECT_EQ(shrinkPixels(small, 64), small);
    // Not square: the aspect is kept.
    const IconPixels wide = shrinkPixels(solid(200, 100, 1, 2, 3, 255), 50);
    EXPECT_EQ(wide.width, 50);
    EXPECT_EQ(wide.height, 25);
}

TEST(IconPixels, TransparentPixelsLendNoColor)
{
    // Half transparent black, half opaque white, side by side: the average is white at half alpha
    // (a straight average would be grey: dark fringes around every icon).
    IconPixels two;
    two.width = 2;
    two.height = 1;
    two.rgba = {0, 0, 0, 0, 255, 255, 255, 255};
    const IconPixels one = shrinkPixels(two, 1);
    ASSERT_EQ(one.rgba.size(), 4u);
    EXPECT_EQ(one.rgba[0], 255);
    EXPECT_EQ(one.rgba[3], 128);
    // Nonsense in, nothing out of bounds.
    IconPixels broken;
    broken.width = 100;
    broken.height = 100;
    broken.rgba.resize(16);
    EXPECT_EQ(shrinkPixels(broken, 10), broken);
}

} // namespace
