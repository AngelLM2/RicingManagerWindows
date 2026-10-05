#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <gtest/gtest.h>

namespace
{

TEST(WinError, MessageHasNoTrailingLineBreak)
{
    const std::string message = rmw::platform::win32Message(ERROR_FILE_NOT_FOUND);
    ASSERT_FALSE(message.empty());
    EXPECT_NE(message.back(), '\n');
    EXPECT_NE(message.back(), '\r');
}

// ERROR_WRONG_DISK's text contains %1/%2/%3 inserts. Without FORMAT_MESSAGE_IGNORE_INSERTS
// FormatMessage would read arguments that do not exist (docs/02 C5).
TEST(WinError, MessagesWithInsertsAreSafe)
{
    const std::string message = rmw::platform::win32Message(ERROR_WRONG_DISK);
    EXPECT_FALSE(message.empty());
}

TEST(WinError, UnknownCodeGetsAFallbackMessage)
{
    EXPECT_NE(rmw::platform::win32Message(0x2FFFFFFF).find("0x2FFFFFFF"), std::string::npos);
}

TEST(WinError, ClassifiesCommonCodes)
{
    EXPECT_EQ(rmw::platform::win32Error(ERROR_ACCESS_DENIED, "x").code(), rmw::ErrorCode::AccessDenied);
    EXPECT_EQ(rmw::platform::win32Error(ERROR_FILE_NOT_FOUND, "x").code(), rmw::ErrorCode::NotFound);
    EXPECT_EQ(rmw::platform::win32Error(ERROR_GEN_FAILURE, "x").code(), rmw::ErrorCode::System);
}

TEST(WinError, UnwrapsWin32HResults)
{
    const rmw::Error error =
        rmw::platform::hresultError(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), "opening key");
    EXPECT_EQ(error.code(), rmw::ErrorCode::AccessDenied);
    EXPECT_EQ(error.message().rfind("opening key: ", 0), 0u);
}

TEST(WinError, LastErrorReadsGetLastError)
{
    SetLastError(ERROR_PATH_NOT_FOUND);
    EXPECT_EQ(rmw::platform::lastError("test").code(), rmw::ErrorCode::NotFound);
}

} // namespace
