#include "Shell.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>

namespace rmw::settingsapp
{
namespace
{

namespace fs = std::filesystem;

std::optional<fs::path> runDialog(void* owner, bool folders, const std::vector<FileFilter>& filters)
{
    auto dialog = wil::CoCreateInstanceNoThrow<IFileOpenDialog>(CLSID_FileOpenDialog);
    if (!dialog)
    {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (folders)
    {
        options |= FOS_PICKFOLDERS;
    }
    else
    {
        options |= FOS_FILEMUSTEXIST;
    }
    dialog->SetOptions(options);
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& filter : filters)
    {
        specs.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }
    if (!specs.empty())
    {
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }
    if (FAILED(dialog->Show(static_cast<HWND>(owner))))
    {
        return std::nullopt; // cancelled
    }
    wil::com_ptr_nothrow<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put())))
    {
        return std::nullopt;
    }
    wil::unique_cotaskmem_string path;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put())))
    {
        return std::nullopt;
    }
    return fs::path(path.get());
}

std::uint64_t toU64(FILETIME time) noexcept
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::string redactProfile(std::string text)
{
    wchar_t* raw = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&raw, &length, L"USERPROFILE") != 0 || raw == nullptr)
    {
        return text;
    }
    const std::unique_ptr<wchar_t, decltype(&std::free)> profile(raw, &std::free);
    const std::string mine = utf16To8Lossy(profile.get());
    // JSON writes the backslashes doubled.
    std::string doubled;
    for (const char c : mine)
    {
        doubled += c;
        if (c == '\\')
        {
            doubled += '\\';
        }
    }
    for (const std::string& needle : {doubled, mine})
    {
        for (std::size_t at = text.find(needle); !needle.empty() && at != std::string::npos;
             at = text.find(needle, at))
        {
            text.replace(at, needle.size(), "%USERPROFILE%");
        }
    }
    return text;
}

} // namespace

std::optional<fs::path> pickFile(void* owner, const std::vector<FileFilter>& filters)
{
    return runDialog(owner, false, filters);
}

std::optional<fs::path> pickFolder(void* owner)
{
    return runDialog(owner, true, {});
}

void openFolder(const fs::path& folder)
{
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void setDarkTitleBar(void* window, bool dark)
{
    if (window == nullptr)
    {
        return;
    }
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(static_cast<HWND>(window), DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
}

bool windowsUsesDarkApps()
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

bool windowsSpeaksPortuguese()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
}

std::vector<ProcessSample> ProcessWatch::sample()
{
    std::vector<ProcessSample> samples;
    const wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE)
    {
        return samples;
    }
    FILETIME nowTime{};
    GetSystemTimeAsFileTime(&nowTime);
    const std::uint64_t now = toU64(nowTime);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const double cores = std::max<DWORD>(system.dwNumberOfProcessors, 1);

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::map<std::uint32_t, Last> seen;
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more != FALSE;
         more = Process32NextW(snapshot.get(), &entry))
    {
        const std::wstring_view name(entry.szExeFile);
        if (!name.starts_with(L"rmw") || !name.ends_with(L".exe"))
        {
            continue;
        }
        ProcessSample sample;
        sample.name = utf16To8Lossy(name);
        sample.pid = entry.th32ProcessID;
        const wil::unique_handle process(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (process)
        {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            if (GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                     sizeof(counters)) != FALSE)
            {
                sample.privateBytes = counters.PrivateUsage;
            }
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) != FALSE)
            {
                const std::uint64_t cpu = toU64(kernel) + toU64(user);
                if (const auto last = m_last.find(sample.pid);
                    last != m_last.end() && now > last->second.at100ns)
                {
                    sample.cpuPercent = 100.0 * static_cast<double>(cpu - last->second.cpu100ns) /
                                        static_cast<double>(now - last->second.at100ns) / cores;
                }
                seen[sample.pid] = {cpu, now};
            }
        }
        samples.push_back(std::move(sample));
    }
    m_last = std::move(seen);
    std::ranges::sort(samples,
                      [](const ProcessSample& a, const ProcessSample& b) { return a.name < b.name; });
    return samples;
}

std::vector<std::string> recentProblems(const fs::path& dataDirectory, std::size_t max)
{
    constexpr std::streamoff kTail = std::streamoff{64} * 1024;
    std::vector<std::string> lines;
    std::error_code ec;
    for (fs::directory_iterator it(dataDirectory / L"logs", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().extension() != L".log")
        {
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(std::max<std::streamoff>(0, size - kTail));
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t stop = text.find('\n', start);
            if (stop == std::string::npos)
            {
                stop = text.size();
            }
            std::string line = text.substr(start, stop - start);
            start = stop + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.size() > 30 && (line.find(" WARN ") == 24 || line.find(" ERROR ") == 24))
            {
                lines.push_back(std::move(line));
            }
        }
    }
    // The lines start with their UTC time: newest first.
    std::ranges::sort(lines, std::greater<>());
    if (lines.size() > max)
    {
        lines.resize(max);
    }
    return lines;
}

Result<fs::path> makeSupportBundle(const fs::path& dataDirectory, std::string_view state)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fs::path folder = dataDirectory / L"support" /
                      std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}", now.wYear, now.wMonth, now.wDay,
                                  now.wHour, now.wMinute, now.wSecond);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec)
    {
        return fail(ErrorCode::Io,
                    std::format("cannot create {}: {}", utf16To8Lossy(folder.native()), ec.message()));
    }
    for (fs::directory_iterator it(dataDirectory / L"logs", ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            // The logs already hide the user's folder (they are written that way).
            [[maybe_unused]] const auto copied =
                platform::copyHashed(it->path(), folder / it->path().filename(), std::uint64_t{16} << 20);
        }
    }
    const std::string redacted = redactProfile(std::string(state));
    if (auto written = platform::writeFileAtomic(folder / L"state.json", std::as_bytes(std::span(redacted)));
        !written)
    {
        return std::unexpected(std::move(written.error()));
    }
    return folder;
}

} // namespace rmw::settingsapp
