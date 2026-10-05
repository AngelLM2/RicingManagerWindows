#include "rmw/platform/Console.hpp"

#include <gtest/gtest.h>

namespace
{

// Under ctest the standard streams are pipes, so this exercises the UTF-8 byte path;
// in a terminal it exercises WriteConsoleW. Either way non-ASCII text must not fail.
TEST(Console, WritesUnicodeLines)
{
    EXPECT_TRUE(rmw::platform::writeLine(rmw::platform::Stream::Out, "console test: Índice 夜景 🎴"));
}

} // namespace
