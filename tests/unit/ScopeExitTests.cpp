#include "rmw/base/ScopeExit.hpp"

#include <gtest/gtest.h>

namespace
{

TEST(ScopeExit, RunsOnScopeExit)
{
    int calls = 0;
    {
        auto guard = rmw::ScopeExit([&]() noexcept { ++calls; });
    }
    EXPECT_EQ(calls, 1);
}

TEST(ScopeExit, ReleaseCancelsTheCall)
{
    int calls = 0;
    {
        auto guard = rmw::ScopeExit([&]() noexcept { ++calls; });
        guard.release();
    }
    EXPECT_EQ(calls, 0);
}

} // namespace
