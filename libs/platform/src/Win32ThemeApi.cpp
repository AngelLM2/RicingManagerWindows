#include "ThemeManagerCom.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <oleauto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <span>
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

// What a theme apply may change that is not the look, kept before it and put back after: every value
// of Control Panel\Cursors (the pointers, their scheme, their size), the pointer style of Accessibility
// (size, type, color), Settings' recent colors and the theme to come back to after high contrast. The legacy theme manager applies a whole theme file,
// its pointer section included (and that section is often older than the pointers in use), and every
// apply adds its accent to the recent colors.
class Kept
{
public:
    static Kept take()
    {
        Kept kept;
        for (const auto& spec : kKeys)
        {
            kept.m_keys.push_back(read(spec));
        }
        return kept;
    }

    // Put back now, and again while Explorer finishes the apply (it writes some of these late).
    void putBackSettled() const
    {
        putBack();
        for (const auto wait : {std::chrono::milliseconds(400), std::chrono::milliseconds(1200)})
        {
            std::this_thread::sleep_for(wait);
            putBack();
        }
    }

    // Writes back what changed, deletes what appeared; the pointers are reloaded when they changed.
    void putBack() const
    {
        bool cursors = false;
        std::optional<DWORD> size;
        for (std::size_t k = 0; k < m_keys.size(); ++k)
        {
            const Key& before = m_keys[k];
            const Key now = read(kKeys[k]);
            HKEY handle = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, before.path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                &handle, nullptr) != ERROR_SUCCESS)
            {
                continue;
            }
            for (const auto& value : before.values)
            {
                const auto there = std::ranges::find(now.values, value.name, &Value::name);
                if (there == now.values.end() || there->type != value.type || there->data != value.data)
                {
                    RegSetValueExW(handle, value.name.c_str(), 0, value.type, value.data.data(),
                                   static_cast<DWORD>(value.data.size()));
                    cursors = cursors || k == 0;
                    if (k == 0 && value.name == L"CursorBaseSize" && value.type == REG_DWORD &&
                        value.data.size() == sizeof(DWORD))
                    {
                        DWORD pixels = 0;
                        std::memcpy(&pixels, value.data.data(), sizeof(pixels));
                        size = pixels;
                    }
                }
            }
            for (const auto& value : now.values)
            {
                if (std::ranges::find(before.values, value.name, &Value::name) == before.values.end())
                {
                    RegDeleteValueW(handle, value.name.c_str());
                    cursors = cursors || k == 0;
                }
            }
            RegCloseKey(handle);
        }
        if (cursors)
        {
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
            if (size)
            {
                constexpr UINT kSetCursorBaseSize = 0x2029; // SPI_SETCURSORBASESIZE
                SystemParametersInfoW(kSetCursorBaseSize, 0,
                                      reinterpret_cast<PVOID>(static_cast<UINT_PTR>(*size)), // NOLINT(performance-no-int-to-ptr)
                                      0);
            }
            log::info("theme: the pointers a theme apply changed are back");
        }
    }

private:
    struct Value
    {
        std::wstring name;
        DWORD type = 0;
        std::vector<BYTE> data;
    };
    struct Key
    {
        std::wstring path;
        std::vector<Value> values;
    };
    struct Spec
    {
        std::wstring_view path;
        std::array<std::wstring_view, 3> names; // `count` of them; none: every value of the key
        std::size_t count = 0;
    };
    static constexpr std::array<Spec, 5> kKeys = {{
        {LR"(Control Panel\Cursors)", {}, 0},
        {LR"(Software\Microsoft\Accessibility)", {L"CursorSize", L"CursorType", L"CursorColor"}, 3},
        {LR"(Software\Microsoft\Windows\CurrentVersion\Themes\History\Colors)", {}, 0},
        {LR"(Software\Microsoft\Windows\CurrentVersion\Themes\History)", {L"AutoColor"}, 1},
        // The theme Windows goes back to after high contrast: the user's, never a theme of ours.
        {LR"(Software\Microsoft\Windows\CurrentVersion\Themes)", {L"Pre-High Contrast Scheme"}, 1},
    }};

    static Key read(const Spec& spec)
    {
        Key key{std::wstring(spec.path), {}};
        const std::span<const std::wstring_view> names(spec.names.data(), spec.count);
        HKEY handle = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.path.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
        {
            return key;
        }
        for (DWORD i = 0; i < 256; ++i)
        {
            std::array<wchar_t, 256> name{};
            auto nameSize = static_cast<DWORD>(name.size());
            std::vector<BYTE> data(4096);
            auto dataSize = static_cast<DWORD>(data.size());
            DWORD type = 0;
            if (RegEnumValueW(handle, i, name.data(), &nameSize, nullptr, &type, data.data(), &dataSize) != ERROR_SUCCESS)
            {
                break;
            }
            const std::wstring valueName(name.data(), nameSize);
            if (!names.empty() && std::ranges::find(names, valueName) == names.end())
            {
                continue;
            }
            data.resize(dataSize);
            key.values.push_back({valueName, type, std::move(data)});
        }
        RegCloseKey(handle);
        return key;
    }

    std::vector<Key> m_keys;
};

// "@%SystemRoot%\System32\themeui.dll,-2013" and the like: the name Settings shows.
std::wstring displayNameOf(const std::filesystem::path& file)
{
    auto theme = readThemeFile(file);
    if (!theme)
    {
        return {};
    }
    bool inTheme = false;
    for (std::size_t start = 0; start <= theme->text.size();)
    {
        const std::size_t end = theme->text.find(L'\n', start);
        const std::wstring_view line =
            trim(std::wstring_view(theme->text).substr(start, (end == std::wstring::npos ? theme->text.size() : end) - start));
        if (line.starts_with(L'['))
        {
            inTheme = equalsIgnoreCase(line, L"[Theme]");
        }
        else if (inTheme)
        {
            const auto equals = line.find(L'=');
            if (equals != std::wstring_view::npos && equalsIgnoreCase(trim(line.substr(0, equals)), L"DisplayName"))
            {
                std::wstring value(trim(line.substr(equals + 1)));
                if (value.starts_with(L'@'))
                {
                    std::array<wchar_t, 512> resolved{};
                    if (SUCCEEDED(SHLoadIndirectString(value.c_str(), resolved.data(), static_cast<UINT>(resolved.size()), nullptr)))
                    {
                        value = resolved.data();
                    }
                }
                return value;
            }
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return {};
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
    // restarts, and switching from light to dark): a turn that does not show is applied again —
    // as the other turn's file, and the last time through the legacy manager. A theme shows in a
    // few hundred ms (S4: ~200 ms); one that has not after 1.2 s waits for the next try.
    constexpr int kTurns = 3;
    constexpr std::array<std::chrono::milliseconds, kTurns - 1> kTurnEnds = {std::chrono::milliseconds(1200),
                                                                             std::chrono::milliseconds(3000)};
    const Kept kept = Kept::take();
    const auto keep = wil::scope_exit([&] { kept.putBackSettled(); });
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

        const auto turnEnd = turn == kTurns
                                 ? deadline
                                 : std::min(deadline, start + kTurnEnds[static_cast<std::size_t>(turn - 1)]);
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

Result<void> Win32ThemeApi::nameCurrentTheme(const std::string& file)
{
    auto wide = utf8To16(file);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    const auto bytes = static_cast<DWORD>((wide->size() + 1) * sizeof(wchar_t));
    // Windows may still be writing these from the last apply: written again a moment later. The theme to
    // come back to after high contrast is Windows' to keep, but never one of ours: that would bring a
    // rice's look back.
    const auto ours = [](const std::wstring& path) {
        const std::wstring name = std::filesystem::path(path).filename().native();
        return name.size() >= 13 && equalsIgnoreCase(std::wstring_view(name).substr(0, 13), L"RicingManager");
    };
    for (int round = 0; round < 2; ++round)
    {
        if (const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, std::wstring(kThemesKey).c_str(),
                                                   L"CurrentTheme", REG_SZ, wide->c_str(), bytes);
            status != ERROR_SUCCESS)
        {
            return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "name the current theme"));
        }
        if (const auto highContrast = readString(kThemesKey, L"Pre-High Contrast Scheme");
            highContrast && ours(*highContrast))
        {
            RegSetKeyValueW(HKEY_CURRENT_USER, std::wstring(kThemesKey).c_str(), L"Pre-High Contrast Scheme", REG_SZ,
                            wide->c_str(), bytes);
        }
        if (round == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    return {};
}

Result<void> Win32ThemeApi::applyThemeFile(const std::string& file)
{
    auto wide = utf8To16(file);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    const std::filesystem::path path(expand(*wide));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return fail(ErrorCode::NotFound, std::format("the theme {} is gone", file));
    }
    const Kept kept = Kept::take();
    const auto keep = wil::scope_exit([&] { kept.putBackSettled(); });
    const std::wstring name = displayNameOf(path);
    HRESULT result = E_FAIL;
    std::thread worker([&] {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init))
        {
            result = init;
            return;
        }
        // The look only (Windows' list knows it by its name); else the whole file, the pointers kept.
        result = name.empty() ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : applyByIndex(name);
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            result = applyLegacy(path);
        }
        CoUninitialize();
    });
    worker.join();
    if (FAILED(result))
    {
        return std::unexpected(hresultError(result, "apply the theme again"));
    }
    // Windows takes it in the background: until it names the file as the current theme.
    const auto deadline = std::chrono::steady_clock::now() + m_wait;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (const auto current = readString(kThemesKey, L"CurrentTheme");
            current && equalsIgnoreCase(expand(*current), path.native()))
        {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    log::warn("theme: Windows does not name {} as the current theme yet", file);
    return {};
}

} // namespace rmw::platform
