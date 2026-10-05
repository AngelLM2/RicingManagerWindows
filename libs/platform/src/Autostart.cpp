#include "rmw/platform/Autostart.hpp"

#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <array>
#include <cwchar>
#include <format>

namespace rmw::platform
{
namespace
{

constexpr const wchar_t* kRunKey = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr const wchar_t* kApprovedKey =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";
constexpr const wchar_t* kValueName = L"RicingManagerWindows";

} // namespace

std::wstring autostartCommand(const std::filesystem::path& executable)
{
    return std::format(L"\"{}\" --startup", executable.native());
}

Result<AutostartState> autostartState(const std::filesystem::path& executable)
{
    AutostartState state;
    std::array<wchar_t, 2048> command{};
    auto bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    const LSTATUS read =
        RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, command.data(), &bytes);
    if (read == ERROR_SUCCESS)
    {
        state.registered = true;
        state.command = command.data();
        state.current = _wcsicmp(state.command.c_str(), autostartCommand(executable).c_str()) == 0;
    }
    else if (read != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(win32Error(static_cast<DWORD>(read), "read the Run entry"));
    }

    // Task Manager keeps a 12-byte record per entry; an odd first byte means "disabled".
    std::array<BYTE, 12> approved{};
    auto approvedBytes = static_cast<DWORD>(approved.size());
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, approved.data(),
                     &approvedBytes) == ERROR_SUCCESS &&
        approvedBytes >= 1)
    {
        state.disabledByUser = (approved[0] & 1U) != 0;
    }
    return state;
}

Result<void> setAutostart(bool enabled, const std::filesystem::path& executable)
{
    if (!enabled)
    {
        const LSTATUS removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
        {
            return std::unexpected(win32Error(static_cast<DWORD>(removed), "remove the Run entry"));
        }
        return {};
    }
    const std::wstring command = autostartCommand(executable);
    const LSTATUS written = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
                                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (written != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<DWORD>(written), "write the Run entry"));
    }
    return {};
}

} // namespace rmw::platform
