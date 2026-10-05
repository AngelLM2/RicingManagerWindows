#include "ThemeManagerCom.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shlobj.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <oleauto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

namespace rmw::platform
{
namespace
{

using namespace detail;

constexpr std::uintmax_t kMaxThemeBytes = 1u << 20;
constexpr std::wstring_view kPersonalize = LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
constexpr std::wstring_view kDwm = LR"(Software\Microsoft\Windows\DWM)";
constexpr std::wstring_view kThemesKey = LR"(Software\Microsoft\Windows\CurrentVersion\Themes)";

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

std::wstring_view trim(std::wstring_view text) noexcept
{
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

struct ThemeFile
{
    std::wstring text;
    bool utf16 = false;
};

Result<ThemeFile> readThemeFile(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxThemeBytes)
    {
        return fail(ErrorCode::InvalidData, "cannot read the active theme file");
    }
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    {
        return fail(ErrorCode::Io, "cannot read the active theme file");
    }

    ThemeFile file;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        file.utf16 = true;
        file.text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                         (bytes.size() - 2) / sizeof(wchar_t));
        return file;
    }
    std::string_view body = bytes;
    UINT codePage = CP_ACP;
    if (body.starts_with("\xEF\xBB\xBF"))
    {
        body.remove_prefix(3);
        codePage = CP_UTF8;
    }
    const int chars =
        MultiByteToWideChar(codePage, 0, body.data(), static_cast<int>(body.size()), nullptr, 0);
    file.text.resize(static_cast<std::size_t>(chars));
    MultiByteToWideChar(codePage, 0, body.data(), static_cast<int>(body.size()), file.text.data(), chars);
    return file;
}

// Same encoding as the base when possible; UTF-16 when ANSI would lose characters.
std::vector<std::byte> encodeThemeFile(const std::wstring& text, bool preferUtf16)
{
    if (!preferUtf16)
    {
        BOOL lossy = FALSE;
        const int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0, nullptr, &lossy);
        if (lossy == FALSE && bytes >= 0)
        {
            std::vector<std::byte> out(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()),
                                reinterpret_cast<char*>(out.data()), bytes, nullptr, nullptr);
            return out;
        }
    }
    std::vector<std::byte> out(2 + text.size() * sizeof(wchar_t));
    out[0] = std::byte{0xFF};
    out[1] = std::byte{0xFE};
    std::memcpy(out.data() + 2, text.data(), text.size() * sizeof(wchar_t));
    return out;
}

std::wstring expand(const std::wstring& text)
{
    const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0)
    {
        return text;
    }
    std::wstring out(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(text.c_str(), out.data(), needed);
    out.resize(written > 0 ? written - 1 : 0);
    return out;
}

std::filesystem::path windowsDirectory()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return {std::wstring(buffer.data(), length)};
}

std::optional<std::wstring> readString(std::wstring_view key, const wchar_t* name)
{
    std::array<wchar_t, 1024> buffer{};
    auto size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, std::wstring(key).c_str(), name,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buffer.data(),
                     &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

std::optional<DWORD> readDword(std::wstring_view key, const wchar_t* name)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, std::wstring(key).c_str(), name, RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return value;
}

bool registryShows(const ports::ThemeLook& look)
{
    const auto apps = readDword(kPersonalize, L"AppsUseLightTheme");
    const auto system = readDword(kPersonalize, L"SystemUsesLightTheme");
    const auto colorization = readDword(kDwm, L"ColorizationColor");
    return apps == (look.appsLight ? 1u : 0u) && system == (look.systemLight ? 1u : 0u) && colorization &&
           (*colorization & 0x00FFFFFF) == (look.colorizationArgb & 0x00FFFFFF);
}

// What we wait for and what Windows shows, for the message when it never comes.
std::string lookDifference(const ports::ThemeLook& look)
{
    const auto shown = [](std::optional<DWORD> value) {
        return value ? std::format("{:X}", *value) : std::string("none");
    };
    return std::format("wanted apps={} system={} color={:06X}; Windows shows apps={} system={} color={}",
                       look.appsLight ? 1 : 0, look.systemLight ? 1 : 0, look.colorizationArgb & 0x00FFFFFF,
                       shown(readDword(kPersonalize, L"AppsUseLightTheme")),
                       shown(readDword(kPersonalize, L"SystemUsesLightTheme")),
                       shown(readDword(kDwm, L"ColorizationColor")));
}

// Fresh manager (it re-reads the Themes folders), find ours by name, apply it by index.
HRESULT applyByIndex(std::wstring_view displayName)
{
    wil::com_ptr_nothrow<IThemeManager2> manager;
    HRESULT hr = CoCreateInstance(kClsidThemeManager2, nullptr, CLSCTX_ALL, __uuidof(IThemeManager2),
                                  manager.put_void());
    if (FAILED(hr))
    {
        return hr;
    }
    hr = manager->Init(0);
    if (FAILED(hr))
    {
        return hr;
    }
    int count = 0;
    hr = manager->GetThemeCount(&count);
    if (FAILED(hr))
    {
        return hr;
    }
    for (int i = 0; i < count; ++i)
    {
        wil::com_ptr_nothrow<ITheme> theme;
        wil::unique_bstr name;
        if (SUCCEEDED(manager->GetTheme(i, theme.put())) && theme &&
            SUCCEEDED(theme->get_DisplayName(name.put())) && name && displayName == name.get())
        {
            return manager->SetCurrentTheme(nullptr, i, 1, kApplyOnlyLook, 0);
        }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT applyLegacy(const std::filesystem::path& path)
{
    wil::com_ptr_nothrow<ILegacyThemeManager> legacy;
    const HRESULT hr = CoCreateInstance(kClsidLegacyThemeManager, nullptr, CLSCTX_ALL,
                                        __uuidof(ILegacyThemeManager), legacy.put_void());
    if (FAILED(hr))
    {
        return hr;
    }
    const wil::unique_bstr bstr(SysAllocString(path.c_str()));
    return bstr ? legacy->ApplyTheme(bstr.get()) : E_OUTOFMEMORY;
}

struct ManagedTheme
{
    std::filesystem::path path;
    std::wstring_view displayName;
    std::filesystem::path other; // the previous turn's file: removed once this one shows
};

Result<ManagedTheme> writeManagedTheme(const ports::ThemeLook& look)
{
    wil::unique_cotaskmem_string localAppData;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, localAppData.put())))
    {
        return fail(ErrorCode::System, "cannot find %LOCALAPPDATA%");
    }
    const std::filesystem::path themes =
        std::filesystem::path(localAppData.get()) / L"Microsoft" / L"Windows" / L"Themes";
    std::error_code ec;
    std::filesystem::create_directories(themes, ec);
    const std::filesystem::path first = themes / L"RicingManager.theme";
    const std::filesystem::path second = themes / L"RicingManager-2.theme";

    // Base: the active theme, so cursors, sounds and wallpaper entries stay the user's.
    std::filesystem::path base;
    if (const auto current = readString(kThemesKey, L"CurrentTheme"))
    {
        base = expand(*current);
    }
    auto file =
        base.empty() ? Result<ThemeFile>(fail(ErrorCode::NotFound, "no active theme")) : readThemeFile(base);
    if (!file)
    {
        log::warn("theme: active theme unreadable ({}); using aero.theme", file.error().message());
        file = readThemeFile(windowsDirectory() / L"Resources" / L"Themes" / L"aero.theme");
        if (!file)
        {
            return std::unexpected(std::move(file.error()));
        }
    }
    // Whichever of the two is not the current theme.
    const bool firstIsCurrent = !base.empty() && equalsIgnoreCase(base.lexically_normal().native(),
                                                                  first.lexically_normal().native());
    ManagedTheme managed = firstIsCurrent ? ManagedTheme{second, kManagedThemeNameB, first}
                                          : ManagedTheme{first, kManagedThemeName, second};
    const std::wstring text = buildManagedTheme(file->text, look, managed.displayName);
    if (auto written = writeFileAtomic(managed.path, encodeThemeFile(text, file->utf16)); !written)
    {
        return std::unexpected(std::move(written.error()).withContext("write the managed theme"));
    }
    return managed;
}

} // namespace

std::wstring buildManagedTheme(std::wstring_view baseTheme, const ports::ThemeLook& look,
                               std::wstring_view displayName)
{
    struct Setting
    {
        std::wstring_view section;
        std::wstring_view key;
        std::wstring value;
        bool done = false;
    };
    std::array<Setting, 4> settings = {{
        {L"Theme", L"DisplayName", std::wstring(displayName)},
        {L"VisualStyles", L"SystemMode", look.systemLight ? L"Light" : L"Dark"},
        {L"VisualStyles", L"AppMode", look.appsLight ? L"Light" : L"Dark"},
        {L"VisualStyles", L"ColorizationColor", std::format(L"0X{:08X}", look.colorizationArgb)},
    }};

    std::vector<std::wstring> lines;
    for (std::size_t start = 0; start <= baseTheme.size();)
    {
        const std::size_t end = baseTheme.find(L'\n', start);
        std::wstring_view line =
            baseTheme.substr(start, (end == std::wstring_view::npos ? baseTheme.size() : end) - start);
        if (line.ends_with(L'\r'))
        {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (end == std::wstring_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    while (!lines.empty() && trim(lines.back()).empty())
    {
        lines.pop_back();
    }

    // Replace existing keys; remember where each section ends to insert missing ones.
    std::wstring section;
    std::vector<std::pair<std::wstring, std::size_t>> sectionEnds; // section -> index after its last line
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring_view line = trim(lines[i]);
        if (line.starts_with(L'[') && line.ends_with(L']'))
        {
            section = std::wstring(line.substr(1, line.size() - 2));
            sectionEnds.emplace_back(section, i + 1);
            continue;
        }
        if (!line.empty() && !sectionEnds.empty())
        {
            sectionEnds.back().second = i + 1;
        }
        const auto equals = line.find(L'=');
        if (equals == std::wstring_view::npos)
        {
            continue;
        }
        const std::wstring_view key = trim(line.substr(0, equals));
        for (auto& setting : settings)
        {
            if (equalsIgnoreCase(section, setting.section) && equalsIgnoreCase(key, setting.key))
            {
                lines[i] = std::format(L"{}={}", setting.key, setting.value);
                setting.done = true;
            }
        }
    }

    // Insert missing keys at the end of their section (from the bottom, so indexes stay valid).
    for (auto it = sectionEnds.rbegin(); it != sectionEnds.rend(); ++it)
    {
        for (auto& setting : settings)
        {
            if (!setting.done && equalsIgnoreCase(it->first, setting.section))
            {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(it->second),
                             std::format(L"{}={}", setting.key, setting.value));
                setting.done = true;
            }
        }
    }
    // Sections that do not exist at all.
    for (const std::wstring_view missing : {std::wstring_view(L"Theme"), std::wstring_view(L"VisualStyles")})
    {
        bool header = false;
        for (auto& setting : settings)
        {
            if (!setting.done && setting.section == missing)
            {
                if (!header)
                {
                    lines.emplace_back();
                    lines.push_back(std::format(L"[{}]", missing));
                    header = true;
                }
                lines.push_back(std::format(L"{}={}", setting.key, setting.value));
                setting.done = true;
            }
        }
    }

    std::wstring out;
    for (const auto& line : lines)
    {
        out += line;
        out += L"\r\n";
    }
    return out;
}

Result<void> Win32ThemeApi::applyLook(const ports::ThemeLook& look)
{
    // Windows sometimes takes a theme and keeps the old accent (seen right after Explorer
    // restarts): a turn that does not show in a third of the time is applied again — as the other
    // turn's file, and the last time through the legacy manager.
    constexpr int kTurns = 3;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + m_wait;
    for (int turn = 1; turn <= kTurns; ++turn)
    {
        auto managed = writeManagedTheme(look);
        if (!managed)
        {
            return std::unexpected(std::move(managed.error()));
        }

        // A dedicated STA thread: the caller's apartment is unknown, and a COM call into Explorer
        // may block (the --apply-helper process timeout covers that case).
        HRESULT result = E_FAIL;
        std::thread worker([&] {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(init))
            {
                result = init;
                return;
            }
            result = turn < kTurns ? applyByIndex(managed->displayName) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                if (turn < kTurns)
                {
                    log::warn("theme: managed theme not listed by IThemeManager2; using the legacy manager");
                }
                result = applyLegacy(managed->path);
            }
            CoUninitialize();
        });
        worker.join();
        if (FAILED(result))
        {
            return std::unexpected(hresultError(result, "apply the managed theme"));
        }

        const auto turnEnd = turn == kTurns ? deadline : std::min(deadline, start + (m_wait * turn / kTurns));
        while (!registryShows(look) && std::chrono::steady_clock::now() < turnEnd)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (registryShows(look))
        {
            if (turn > 1)
            {
                log::info("theme: shown on try {}", turn);
            }
            // The other turn's file goes: Settings lists one "Ricing Manager" theme, the current one.
            std::error_code ec;
            std::filesystem::remove(managed->other, ec);
            return {};
        }
        if (turn < kTurns)
        {
            log::warn("theme: not shown yet ({}); applying it again", lookDifference(look));
        }
    }
    return fail(ErrorCode::Timeout,
                std::format("Windows did not report the new theme in time ({})", lookDifference(look)));
}

} // namespace rmw::platform
