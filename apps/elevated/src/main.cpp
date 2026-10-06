// rmw-elevated.exe — Ricing Manager's administrator part (doc 10 §4.1, ADR-0021). The core starts it
// through the UAC for ONE operation, and it ends:
//   rmw-elevated set-shell-icons [--folder <sha256> <file>] [--drive <sha256> <file>]
//   rmw-elevated restore-machine-defaults
// A closed list of commands: nothing the caller gives becomes a registry key or a destination path.
// The icons go to %ProgramData%\RicingManagerWindows\machine\icons (only administrators write there;
// every user reads), read once from a handle that refuses links and reparse points, checked against
// the hash given and as an .ico, and written from those same bytes. Before the first change, what
// HKLM\...\Explorer\Shell Icons had is kept in a journal there, which restore-machine-defaults puts
// back. Exit codes: 0 done, 2 a bad command, 3 a bad file, 4 Windows refused, 5 not administrator.

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/CursorFiles.hpp"
#include "rmw/machine/MachineIcons.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/ProcessHardening.hpp"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace rmw;
namespace fs = std::filesystem;

constexpr int kBadCommand = 2;
constexpr int kBadFile = 3;
constexpr int kRefused = 4;
constexpr int kNotAdmin = 5;

// SYSTEM and Administrators: everything; Users: read. Not inherited from above (a folder a user made
// before us loses what that user could do in it).
constexpr const wchar_t* kFolderSddl = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

bool elevated() noexcept
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    {
        return false;
    }
    std::array<std::byte, 128> buffer{};
    DWORD size = 0;
    bool high = false;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &size) !=
        FALSE)
    {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
        high = count > 0 && *GetSidSubAuthority(label->Label.Sid, count - 1) >= SECURITY_MANDATORY_HIGH_RID;
    }
    CloseHandle(token);
    return high;
}

// Junctions and links a user made are not followed (Windows 11 22H2+; older ones keep the checks
// below).
void redirectionGuard() noexcept
{
    PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY policy{};
    policy.EnforceRedirectionTrust = 1;
    SetProcessMitigationPolicy(ProcessRedirectionTrustPolicy, &policy, sizeof(policy));
}

std::optional<fs::path> programData()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &path)))
    {
        return std::nullopt;
    }
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

bool isReparsePoint(const fs::path& path) noexcept
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// A folder of ours: made with our DACL, or made ours (owner and DACL) if someone made it before.
Result<void> ourFolder(const fs::path& folder)
{
    if (isReparsePoint(folder))
    {
        return fail(ErrorCode::AccessDenied, std::format("{} is a link", utf16To8Lossy(folder.native())));
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kFolderSddl, SDDL_REVISION_1, &descriptor, nullptr) ==
        FALSE)
    {
        return fail(ErrorCode::System, "security descriptor", GetLastError());
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Result<void> result;
    if (CreateDirectoryW(folder.c_str(), &attributes) == FALSE)
    {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            result = fail(ErrorCode::Io, "cannot make our folder", GetLastError());
        }
        else
        {
            BOOL present = FALSE;
            BOOL defaulted = FALSE;
            PACL dacl = nullptr;
            PSID owner = nullptr;
            GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
            GetSecurityDescriptorOwner(descriptor, &owner, &defaulted);
            std::wstring name = folder.native();
            const DWORD set = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                                        PROTECTED_DACL_SECURITY_INFORMATION,
                                                    owner, nullptr, dacl, nullptr);
            if (set != ERROR_SUCCESS)
            {
                result = fail(ErrorCode::AccessDenied, "cannot make the folder ours", set);
            }
        }
    }
    LocalFree(descriptor);
    return result;
}

// The source, read once from a handle: a regular file with one name, no link, the size limit; its
// bytes must hash to `sha256` and be a valid .ico.
Result<std::vector<std::byte>> readIcon(const machine::IconRequest& request)
{
    HANDLE file = CreateFileW(request.source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return fail(ErrorCode::NotFound, "cannot open the icon", GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION info{};
    std::vector<std::byte> bytes;
    Result<void> state;
    if (GetFileInformationByHandle(file, &info) == FALSE)
    {
        state = fail(ErrorCode::Io, "cannot read the icon's facts", GetLastError());
    }
    else if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
             info.nNumberOfLinks != 1)
    {
        state = fail(ErrorCode::AccessDenied, "the icon is a link, a folder or has more than one name");
    }
    else
    {
        const std::uint64_t size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
        if (size == 0 || size > limits::kIconMaxBytes)
        {
            state = fail(ErrorCode::LimitExceeded, "the icon is empty or too large");
        }
        else
        {
            bytes.resize(static_cast<std::size_t>(size));
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(size), &read, nullptr) == FALSE || read != size)
            {
                state = fail(ErrorCode::Io, "cannot read the icon", GetLastError());
            }
        }
    }
    CloseHandle(file);
    if (!state)
    {
        return std::unexpected(std::move(state.error()));
    }
    auto digest = platform::sha256(bytes);
    if (!digest)
    {
        return std::unexpected(std::move(digest.error()));
    }
    if (platform::toHex(*digest) != request.sha256)
    {
        return fail(ErrorCode::InvalidData, "the icon is not the one asked for (its hash differs)");
    }
    if (auto valid = domain::validateIconFile(bytes); !valid)
    {
        return std::unexpected(std::move(valid.error()));
    }
    return bytes;
}

// Written from the bytes checked; a name that was there (even a link someone put) is deleted first.
Result<fs::path> storeIcon(const fs::path& folder, const machine::IconRequest& request,
                           std::span<const std::byte> bytes)
{
    fs::path target = folder / machine::storedName(request.sha256);
    DeleteFileW(target.c_str());
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return fail(ErrorCode::Io, "cannot write the icon", GetLastError());
    }
    auto written = platform::writeAll(file, bytes);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!written)
    {
        DeleteFileW(target.c_str());
        return std::unexpected(std::move(written.error()));
    }
    return target;
}

struct Key
{
    HKEY handle = nullptr;
    ~Key()
    {
        if (handle != nullptr)
        {
            RegCloseKey(handle);
        }
    }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    Key(Key&&) = delete;
    Key& operator=(Key&&) = delete;
};

std::wstring keyPath()
{
    return std::wstring(machine::kShellIconsKey);
}

// What Shell Icons has now (absent values left out).
machine::Journal currentValues()
{
    machine::Journal journal;
    Key key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath().c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.handle) !=
        ERROR_SUCCESS)
    {
        return journal;
    }
    journal.keyExisted = true;
    for (const std::wstring_view name : machine::allShellIconValues())
    {
        std::array<wchar_t, 1024> data{};
        DWORD size = sizeof(data) - sizeof(wchar_t);
        DWORD type = 0;
        const std::wstring valueName(name);
        if (RegQueryValueExW(key.handle, valueName.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(data.data()),
                             &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            journal.values.push_back({valueName, type == REG_EXPAND_SZ, std::wstring(data.data())});
        }
    }
    return journal;
}

Result<std::string> readText(const fs::path& path)
{
    if (isReparsePoint(path))
    {
        return fail(ErrorCode::AccessDenied, "the journal is a link");
    }
    auto bytes = platform::readFileLimited(path, std::uint64_t{64} << 10);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

int setShellIcons(const machine::Command& command, const fs::path& home)
{
    const fs::path icons = home / L"icons";
    if (auto made = ourFolder(icons); !made)
    {
        log::error("rmw-elevated: {}", made.error().toString());
        return kRefused;
    }
    // Read and check every icon before anything changes.
    std::vector<std::pair<const machine::IconRequest*, fs::path>> stored;
    for (const auto& request : command.icons)
    {
        auto bytes = readIcon(request);
        if (!bytes)
        {
            log::error("rmw-elevated: {}: {}", domain::riceName(request.icon), bytes.error().toString());
            return kBadFile;
        }
        auto target = storeIcon(icons, request, *bytes);
        if (!target)
        {
            log::error("rmw-elevated: {}", target.error().toString());
            return kRefused;
        }
        stored.emplace_back(&request, std::move(*target));
    }
    // What was there before the first change: kept once, so the restore goes back to Windows' own.
    const fs::path journalFile = home / L"journal.json";
    if (!fs::exists(journalFile))
    {
        const std::string text = machine::toJson(currentValues());
        if (auto written = platform::writeFileAtomic(journalFile, std::as_bytes(std::span(text))); !written)
        {
            log::error("rmw-elevated: the journal: {}", written.error().toString());
            return kRefused;
        }
    }
    Key key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath().c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, &key.handle, nullptr) != ERROR_SUCCESS)
    {
        log::error("rmw-elevated: cannot open Shell Icons");
        return kRefused;
    }
    for (const auto& [request, path] : stored)
    {
        const std::wstring data = path.native() + L",0";
        for (const std::wstring_view name : machine::shellIconValues(request->icon))
        {
            const std::wstring valueName(name);
            if (RegSetValueExW(key.handle, valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                               static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
            {
                log::error("rmw-elevated: cannot set Shell Icons value {}", utf16To8Lossy(valueName));
                return kRefused;
            }
        }
        log::info("rmw-elevated: {} icon set", domain::riceName(request->icon));
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return 0;
}

int restoreDefaults(const fs::path& home)
{
    const fs::path journalFile = home / L"journal.json";
    if (!fs::exists(journalFile))
    {
        log::info("rmw-elevated: nothing to put back");
        return 0;
    }
    auto text = readText(journalFile);
    auto journal = text ? machine::parseJournal(*text) : Result<machine::Journal>(std::unexpected(text.error()));
    if (!journal)
    {
        log::error("rmw-elevated: the journal: {}", journal.error().toString());
        return kRefused;
    }
    {
        Key key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath().c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                          &key.handle) == ERROR_SUCCESS)
        {
            for (const std::wstring_view name : machine::allShellIconValues())
            {
                const std::wstring valueName(name);
                const auto saved = std::ranges::find(journal->values, valueName, &machine::SavedValue::name);
                if (saved == journal->values.end())
                {
                    RegDeleteValueW(key.handle, valueName.c_str());
                }
                else
                {
                    RegSetValueExW(key.handle, valueName.c_str(), 0, saved->expand ? REG_EXPAND_SZ : REG_SZ,
                                   reinterpret_cast<const BYTE*>(saved->data.c_str()),
                                   static_cast<DWORD>((saved->data.size() + 1) * sizeof(wchar_t)));
                }
            }
            DWORD values = 0;
            DWORD subkeys = 0;
            RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr,
                             nullptr, nullptr, nullptr);
            if (!journal->keyExisted && values == 0 && subkeys == 0)
            {
                RegCloseKey(key.handle);
                key.handle = nullptr;
                RegDeleteKeyExW(HKEY_LOCAL_MACHINE, keyPath().c_str(), KEY_WOW64_64KEY, 0);
            }
        }
    }
    // Our icons and the journal go: nothing of ours stays on the machine.
    std::error_code ec;
    const fs::path icons = home / L"icons";
    if (!isReparsePoint(icons))
    {
        for (fs::directory_iterator it(icons, ec), end; !ec && it != end; it.increment(ec))
        {
            DeleteFileW(it->path().c_str());
        }
        RemoveDirectoryW(icons.c_str());
    }
    DeleteFileW(journalFile.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    log::info("rmw-elevated: Windows' own folder and drive icons are back");
    return 0;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program's entry point
int wmain(int argc, wchar_t** argv)
{
    [[maybe_unused]] const auto hardened = platform::hardenProcess();
    redirectionGuard();
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    const auto command = machine::parseCommand(args);
    if (!command)
    {
        return kBadCommand;
    }
    if (!elevated())
    {
        return kNotAdmin;
    }
    const auto data = programData();
    if (!data)
    {
        return kRefused;
    }
    // %ProgramData%\RicingManagerWindows\machine: ours, whoever made the folders above it.
    const fs::path root = *data / L"RicingManagerWindows";
    const fs::path home = root / L"machine";
    for (const auto& folder : {root, home})
    {
        if (auto made = ourFolder(folder); !made)
        {
            return kRefused;
        }
    }
    log::RotatingFileOptions options;
    options.path = home / L"elevated.log";
    if (auto sink = log::makeRotatingFileSink(std::move(options)))
    {
        log::instance().addSink(std::move(*sink));
    }
    try
    {
        return command->kind == machine::Command::Kind::SetShellIcons ? setShellIcons(*command, home)
                                                                     : restoreDefaults(home);
    }
    catch (const std::exception& e)
    {
        log::error("rmw-elevated: {}", e.what());
        return kRefused;
    }
}
