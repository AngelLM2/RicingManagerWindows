#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace rmw::test
{

// A fresh directory under %TEMP%\rmw-tests, removed when the object goes out of scope.
class TempDir
{
public:
    TempDir()
    {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / "rmw-tests" /
                 std::format("{}-{}", stamp, counter.fetch_add(1));
        std::filesystem::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace rmw::test
