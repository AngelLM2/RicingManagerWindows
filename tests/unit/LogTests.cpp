#include "TempDir.hpp"

#include "rmw/base/Log.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

class CaptureSink final : public rmw::log::ISink
{
public:
    explicit CaptureSink(std::vector<std::string>& lines)
        : m_lines(lines)
    {
    }
    void write(rmw::log::Level /*level*/, std::string_view line) noexcept override
    {
        m_lines.emplace_back(line);
    }

private:
    std::vector<std::string>& m_lines;
};

std::string readFile(const std::filesystem::path& path)
{
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

class LogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        rmw::log::instance().clearSinks();
        rmw::log::instance().setLevel(rmw::log::Level::Trace);
        rmw::log::instance().addSink(std::make_unique<CaptureSink>(m_lines));
    }
    void TearDown() override
    {
        rmw::log::instance().clearSinks();
        rmw::log::instance().setLevel(rmw::log::Level::Info);
    }

    std::vector<std::string> m_lines;
};

TEST_F(LogTest, FormatsMessageLevelAndLocation)
{
    rmw::log::info("applied {} in {} ms", "rice", 42);
    ASSERT_EQ(m_lines.size(), 1u);
    EXPECT_NE(m_lines[0].find("INFO"), std::string::npos);
    EXPECT_NE(m_lines[0].find("applied rice in 42 ms"), std::string::npos);
    EXPECT_NE(m_lines[0].find("LogTests.cpp:"), std::string::npos);
}

TEST_F(LogTest, RespectsLevel)
{
    rmw::log::instance().setLevel(rmw::log::Level::Warn);
    rmw::log::info("hidden");
    rmw::log::warn("shown");
    ASSERT_EQ(m_lines.size(), 1u);
    EXPECT_NE(m_lines[0].find("shown"), std::string::npos);
}

TEST_F(LogTest, EscapesLineBreaksSoEntriesCannotBeForged)
{
    rmw::log::warn("first\nERROR fake entry\r");
    ASSERT_EQ(m_lines.size(), 1u);
    EXPECT_EQ(m_lines[0].find('\n'), std::string::npos);
    EXPECT_NE(m_lines[0].find("first\\nERROR fake entry\\r"), std::string::npos);
}

TEST_F(LogTest, RedactsUserProfileCaseInsensitively)
{
    rmw::log::instance().addRedaction(R"(C:\Users\Joao)", "%USERPROFILE%");
    rmw::log::info(R"(loaded c:\users\JOAO\Pictures\a.png)");
    ASSERT_EQ(m_lines.size(), 1u);
    EXPECT_NE(m_lines[0].find(R"(%USERPROFILE%\Pictures\a.png)"), std::string::npos);
    EXPECT_EQ(m_lines[0].find("JOAO"), std::string::npos);
}

TEST_F(LogTest, TruncatesHugeMessagesAtACharacterBoundary)
{
    const std::string huge(rmw::limits::kLogMessageMaxBytes + 100, 'x');
    rmw::log::info("{}", huge);
    ASSERT_EQ(m_lines.size(), 1u);
    EXPECT_NE(m_lines[0].find("[truncated]"), std::string::npos);
    EXPECT_LT(m_lines[0].size(), rmw::limits::kLogMessageMaxBytes + 200);
}

TEST(RotatingFileSink, WritesUtf8AndRotates)
{
    const rmw::test::TempDir dir;
    const auto logPath = dir.path() / "logs" / "core.log";

    auto sink = rmw::log::makeRotatingFileSink({.path = logPath, .maxBytes = 200, .maxFiles = 3});
    ASSERT_TRUE(sink.has_value()) << sink.error().toString();

    const std::string line(60, 'a');
    for (int i = 0; i < 12; ++i)
    {
        (*sink)->write(rmw::log::Level::Info, line);
    }
    (*sink)->write(rmw::log::Level::Warn, "夜景 ok");
    (*sink)->flush();
    sink->reset();

    EXPECT_TRUE(std::filesystem::exists(logPath));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "logs" / "core.1.log"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "logs" / "core.2.log"));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "logs" / "core.3.log"));
    EXPECT_LE(std::filesystem::file_size(logPath), 200u);
    EXPECT_NE(readFile(logPath).find("夜景 ok"), std::string::npos);
}

TEST(RotatingFileSink, RejectsInvalidOptions)
{
    EXPECT_FALSE(rmw::log::makeRotatingFileSink({.path = {}}).has_value());
    EXPECT_FALSE(rmw::log::makeRotatingFileSink({.path = "x.log", .maxBytes = 0}).has_value());
}

} // namespace
