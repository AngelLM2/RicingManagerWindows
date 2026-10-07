#include "AdminChoice.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <format>
#include <span>
#include <string>

namespace rmw::tilerapp
{
namespace
{

constexpr std::uint64_t kMaxBytes = 64;

std::filesystem::path fileFor(const std::filesystem::path& planFile)
{
    return planFile.parent_path() / L"admin-declined";
}

// This sign-in: the logon session of the process's token (a new one at every sign-in).
std::string logonSession()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return {};
    }
    TOKEN_STATISTICS statistics{};
    DWORD size = 0;
    const bool read =
        GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &size) != FALSE;
    CloseHandle(token);
    if (!read)
    {
        return {};
    }
    return std::format("{:08x}{:08x}", static_cast<std::uint32_t>(statistics.AuthenticationId.HighPart),
                       static_cast<std::uint32_t>(statistics.AuthenticationId.LowPart));
}

} // namespace

bool adminDeclinedThisSession(const std::filesystem::path& planFile)
{
    const std::string session = logonSession();
    auto bytes = platform::readFileLimited(fileFor(planFile), kMaxBytes);
    return !session.empty() && bytes &&
           std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()) == session;
}

void rememberAdminDeclined(const std::filesystem::path& planFile)
{
    const std::string session = logonSession();
    if (session.empty())
    {
        return;
    }
    if (auto written = platform::writeFileAtomic(fileFor(planFile), std::as_bytes(std::span(session)),
                                                 platform::Durability::Cached);
        !written)
    {
        log::warn("tiler: cannot remember the administrator answer: {}", written.error().toString());
    }
}

void forgetAdminDeclined(const std::filesystem::path& planFile)
{
    std::error_code ec;
    std::filesystem::remove(fileFor(planFile), ec);
}

} // namespace rmw::tilerapp
