#include "rmw/platform/Win32Ports.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <filesystem>
#include <format>

namespace rmw::platform
{
namespace
{

namespace fs = std::filesystem;

constexpr std::size_t kMaxShortcuts = 2000;
constexpr int kMaxDepth = 4;
constexpr const wchar_t* kSection = L".ShellClassInfo";
constexpr const wchar_t* kIconKey = L"IconResource";

struct FolderName
{
    std::string_view name;
    const KNOWNFOLDERID* id;
};

constexpr std::array<FolderName, 6> kFolders = {{
    {"desktop", &FOLDERID_Desktop},
    {"documents", &FOLDERID_Documents},
    {"downloads", &FOLDERID_Downloads},
    {"pictures", &FOLDERID_Pictures},
    {"music", &FOLDERID_Music},
    {"videos", &FOLDERID_Videos},
}};

Result<fs::path> knownFolder(const KNOWNFOLDERID& id)
{
    wil::unique_cotaskmem_string path;
    if (const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path); FAILED(hr))
    {
        return fail(ErrorCode::System, "cannot find a user folder", hr);
    }
    return fs::path(path.get());
}

Result<fs::path> userFolder(std::string_view name)
{
    const auto found = std::ranges::find(kFolders, name, &FolderName::name);
    if (found == kFolders.end())
    {
        return fail(ErrorCode::InvalidArgument, std::format("unknown user folder '{}'", name));
    }
    return knownFolder(*found->id);
}

// COM on this thread for the call (the apply helper's thread may not have it).
class ComScope
{
public:
    ComScope() noexcept
        : m_ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
    }
    ~ComScope()
    {
        if (m_ok)
        {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

private:
    bool m_ok;
};

std::wstring lowerWide(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// A shortcut's target and icon; nullopt when it cannot be read (a broken file is skipped).
std::optional<ports::Shortcut> readShortcut(const fs::path& file)
{
    auto link = wil::CoCreateInstanceNoThrow<IShellLinkW>(CLSID_ShellLink);
    if (!link)
    {
        return std::nullopt;
    }
    auto persist = link.try_query<IPersistFile>();
    if (!persist || FAILED(persist->Load(file.c_str(), STGM_READ)))
    {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH * 2> target{};
    if (FAILED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)))
    {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH * 2> icon{};
    int index = 0;
    if (FAILED(link->GetIconLocation(icon.data(), static_cast<int>(icon.size()), &index)))
    {
        icon[0] = L'\0';
    }
    ports::Shortcut shortcut;
    shortcut.path = utf16To8Lossy(file.native());
    const fs::path targetPath(target.data());
    const std::wstring name = lowerWide(targetPath.filename().native());
    if (name.ends_with(L".exe"))
    {
        shortcut.program = utf16To8Lossy(name);
    }
    if (icon[0] != L'\0')
    {
        shortcut.icon = std::format("{},{}", utf16To8Lossy(icon.data()), index);
    }
    return shortcut;
}

void collect(const fs::path& folder, int depth, std::vector<ports::Shortcut>& out)
{
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end && out.size() < kMaxShortcuts; it.increment(ec))
    {
        if (it->is_symlink(ec))
        {
            continue;
        }
        if (it->is_directory(ec))
        {
            if (depth > 0)
            {
                collect(it->path(), depth - 1, out);
            }
            continue;
        }
        if (lowerWide(it->path().extension().native()) == L".lnk")
        {
            if (auto shortcut = readShortcut(it->path()))
            {
                out.push_back(std::move(*shortcut));
            }
        }
    }
}

} // namespace

Result<std::optional<std::string>> Win32Icons::folderIcon(std::string_view folder)
{
    auto path = userFolder(folder);
    if (!path)
    {
        return std::unexpected(std::move(path.error()));
    }
    const fs::path ini = *path / L"desktop.ini";
    std::array<wchar_t, MAX_PATH * 2> value{};
    GetPrivateProfileStringW(kSection, kIconKey, L"", value.data(), static_cast<DWORD>(value.size()),
                             ini.c_str());
    if (value[0] == L'\0')
    {
        return std::optional<std::string>();
    }
    return std::optional<std::string>(utf16To8Lossy(value.data()));
}

Result<void> Win32Icons::setFolderIcon(std::string_view folder, const std::optional<std::string>& icon)
{
    auto path = userFolder(folder);
    if (!path)
    {
        return std::unexpected(std::move(path.error()));
    }
    const fs::path ini = *path / L"desktop.ini";
    std::error_code ec;
    const bool existed = fs::exists(ini, ec);
    std::wstring wide;
    if (icon)
    {
        auto converted = utf8To16(*icon);
        if (!converted)
        {
            return std::unexpected(std::move(converted.error()));
        }
        wide = std::move(*converted);
    }
    // nullptr removes the key: the folder's own icon again.
    if (WritePrivateProfileStringW(kSection, kIconKey, icon ? wide.c_str() : nullptr, ini.c_str()) == FALSE)
    {
        return fail(ErrorCode::System, std::format("cannot change the icon of {}", folder), GetLastError());
    }
    if (icon)
    {
        // Explorer reads a desktop.ini only in a folder marked so, and it hides the file itself.
        if (!existed)
        {
            SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
        // (The read-only attribute on a folder means "read its desktop.ini" to Explorer.)
        if (const DWORD attributes = GetFileAttributesW(path->c_str());
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) == 0)
        {
            SetFileAttributesW(path->c_str(), attributes | FILE_ATTRIBUTE_READONLY);
        }
    }
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path->c_str(), nullptr);
    return {};
}

Result<std::vector<ports::Shortcut>> Win32Icons::shortcuts()
{
    const ComScope com;
    std::vector<ports::Shortcut> out;
    // The user's own Start menu, desktop and taskbar pins (never all users': not ours to change).
    if (auto programs = knownFolder(FOLDERID_Programs))
    {
        collect(*programs, kMaxDepth, out);
    }
    if (auto desktop = knownFolder(FOLDERID_Desktop))
    {
        collect(*desktop, 0, out);
    }
    if (auto pinned = knownFolder(FOLDERID_UserPinned))
    {
        collect(*pinned / L"TaskBar", 0, out);
    }
    return out;
}

Result<void> Win32Icons::setShortcutIcon(const std::string& path, const std::string& icon)
{
    const ComScope com;
    auto file = utf8To16(path);
    if (!file)
    {
        return std::unexpected(std::move(file.error()));
    }
    auto link = wil::CoCreateInstanceNoThrow<IShellLinkW>(CLSID_ShellLink);
    auto persist = link ? link.try_query<IPersistFile>() : nullptr;
    if (!persist || FAILED(persist->Load(file->c_str(), STGM_READWRITE)))
    {
        return fail(ErrorCode::Io, std::format("cannot open the shortcut {}", path));
    }
    if (icon.empty())
    {
        // The program's own icon: the shortcut stops naming one.
        if (auto data = link.try_query<IShellLinkDataList>())
        {
            DWORD flags = 0;
            if (SUCCEEDED(data->GetFlags(&flags)))
            {
                data->SetFlags(flags & ~static_cast<DWORD>(SLDF_HAS_ICONLOCATION));
            }
        }
        link->SetIconLocation(L"", 0);
    }
    else
    {
        const auto comma = icon.rfind(',');
        int index = 0;
        std::string location = icon;
        if (comma != std::string::npos)
        {
            const std::string_view number(icon.data() + comma + 1, icon.size() - comma - 1);
            if (std::from_chars(number.data(), number.data() + number.size(), index).ec == std::errc{})
            {
                location = icon.substr(0, comma);
            }
        }
        auto wide = utf8To16(location);
        if (!wide)
        {
            return std::unexpected(std::move(wide.error()));
        }
        link->SetIconLocation(wide->c_str(), index);
    }
    if (const HRESULT hr = persist->Save(file->c_str(), TRUE); FAILED(hr))
    {
        return fail(ErrorCode::Io, std::format("cannot save the shortcut {}", path), hr);
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, file->c_str(), nullptr);
    return {};
}

Result<std::string> Win32Icons::progIdOf(std::string_view extension)
{
    auto wide = utf8To16("." + std::string(extension));
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    std::array<wchar_t, 256> progId{};
    auto size = static_cast<DWORD>(progId.size());
    if (FAILED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_PROGID, wide->c_str(), nullptr, progId.data(), &size)))
    {
        return std::string(); // no program for it
    }
    std::string name = utf16To8Lossy(progId.data());
    // It becomes part of a registry path: a name, nothing else.
    const bool plain = !name.empty() && name.size() < 200 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-' || c == '!' || c == '+';
    });
    // "Unknown" is Windows' class for every type no program opens: its icon is theirs all.
    if (!plain || CompareStringOrdinal(progId.data(), -1, L"Unknown", -1, TRUE) == CSTR_EQUAL)
    {
        return std::string();
    }
    return name;
}

bool Win32Icons::exists(std::string_view location)
{
    std::string file(location);
    // "C:\x.dll,-5": the index is not part of the file.
    if (const auto comma = file.rfind(','); comma != std::string::npos)
    {
        int index = 0;
        const char* const first = file.data() + comma + 1;
        const char* const last = file.data() + file.size();
        if (const auto [end, error] = std::from_chars(first, last, index); error == std::errc{} && end == last)
        {
            file.resize(comma);
        }
    }
    if (file.size() >= 2 && file.front() == '"' && file.back() == '"')
    {
        file = file.substr(1, file.size() - 2);
    }
    auto wide = utf8To16(file);
    if (!wide || wide->empty())
    {
        return false;
    }
    std::array<wchar_t, MAX_PATH * 2> expanded{};
    const DWORD length = ExpandEnvironmentStringsW(wide->c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (length == 0 || length > expanded.size())
    {
        return false;
    }
    return GetFileAttributesW(expanded.data()) != INVALID_FILE_ATTRIBUTES;
}

void Win32Icons::refresh()
{
    // Explorer drops its icon cache and draws every icon again.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

} // namespace rmw::platform
