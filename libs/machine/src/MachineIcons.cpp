#include "rmw/machine/MachineIcons.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/json/Json.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace rmw::machine
{
namespace
{

using json::Json;

constexpr std::array<std::wstring_view, 2> kFolderValues = {L"3", L"4"};
constexpr std::array<std::wstring_view, 1> kDriveValues = {L"8"};
constexpr std::array<std::wstring_view, 3> kAllValues = {L"3", L"4", L"8"};
constexpr std::size_t kMaxPathChars = 1024;
constexpr std::size_t kMaxJournalBytes = std::size_t{64} * 1024;

bool isSha256(std::string_view text) noexcept
{
    return text.size() == 64 &&
           std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isPlainAbsolute(std::wstring_view path) noexcept
{
    // "C:\..." only: no device paths, no UNC, no quotes (it goes on a command line).
    return path.size() > 3 && path.size() < kMaxPathChars && path[1] == L':' && path[2] == L'\\' &&
           ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path.find(L'"') == std::wstring_view::npos && path.find(L"\\\\") == std::wstring_view::npos &&
           std::ranges::none_of(path, [](wchar_t c) { return c < 32; });
}

template <class T>
Result<T> bad(std::string_view what)
{
    return fail(ErrorCode::InvalidArgument, std::format("rmw-elevated: {}", what));
}

} // namespace

std::span<const std::wstring_view> shellIconValues(domain::MachineIcon icon) noexcept
{
    if (icon == domain::MachineIcon::Drive)
    {
        return kDriveValues;
    }
    return kFolderValues;
}

std::span<const std::wstring_view> allShellIconValues() noexcept
{
    return kAllValues;
}

Result<Command> parseCommand(std::span<const std::wstring> args)
{
    if (args.empty())
    {
        return bad<Command>("no command");
    }
    Command command;
    if (args[0] == L"restore-machine-defaults")
    {
        if (args.size() != 1)
        {
            return bad<Command>("restore-machine-defaults takes nothing more");
        }
        command.kind = Command::Kind::RestoreDefaults;
        return command;
    }
    if (args[0] != L"set-shell-icons")
    {
        return bad<Command>("unknown command");
    }
    command.kind = Command::Kind::SetShellIcons;
    for (std::size_t i = 1; i < args.size(); i += 3)
    {
        if (i + 2 >= args.size())
        {
            return bad<Command>("an icon needs --<name> <sha256> <file>");
        }
        const std::wstring_view flag = args[i];
        const auto icon = flag.starts_with(L"--") ? domain::machineIconFromRiceName(utf16To8Lossy(flag.substr(2)))
                                                  : std::nullopt;
        if (!icon || std::ranges::any_of(command.icons, [&](const IconRequest& r) { return r.icon == *icon; }))
        {
            return bad<Command>("unknown or repeated icon");
        }
        const std::string sha = utf16To8Lossy(args[i + 1]);
        if (!isSha256(sha) || !isPlainAbsolute(args[i + 2]))
        {
            return bad<Command>("an icon's hash or file is malformed");
        }
        command.icons.push_back({*icon, sha, args[i + 2]});
    }
    if (command.icons.empty())
    {
        return bad<Command>("set-shell-icons needs an icon");
    }
    return command;
}

Result<std::wstring> commandLine(const Command& command)
{
    if (command.kind == Command::Kind::RestoreDefaults)
    {
        return std::wstring(L"restore-machine-defaults");
    }
    std::wstring line = L"set-shell-icons";
    for (const auto& request : command.icons)
    {
        if (!isSha256(request.sha256) || !isPlainAbsolute(request.source))
        {
            return bad<std::wstring>("an icon's hash or file is malformed");
        }
        line += std::format(L" --{} {} \"{}\"", utf8To16Lossy(domain::riceName(request.icon)),
                            utf8To16Lossy(request.sha256), request.source);
    }
    return line;
}

std::string toJson(const Journal& journal)
{
    Json values = Json::array();
    for (const auto& value : journal.values)
    {
        values.push_back({{"name", utf16To8Lossy(value.name)},
                          {"type", value.expand ? "expand" : "string"},
                          {"data", utf16To8Lossy(value.data)}});
    }
    return json::dumpPretty({{"version", 1}, {"keyExisted", journal.keyExisted}, {"values", std::move(values)}});
}

Result<Journal> parseJournal(std::string_view text)
{
    auto parsed = json::parse(text, {.maxBytes = kMaxJournalBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    if (!root.is_object() || json::intOr(root, "version", 0) != 1)
    {
        return fail(ErrorCode::InvalidData, "machine journal: not version 1");
    }
    Journal journal;
    journal.keyExisted = json::boolOr(root, "keyExisted", false);
    const Json values = json::memberOr(root, "values", Json::array());
    if (!values.is_array() || values.size() > kAllValues.size())
    {
        return fail(ErrorCode::InvalidData, "machine journal: values");
    }
    for (const auto& entry : values)
    {
        const std::wstring name = utf8To16Lossy(json::stringOr(entry, "name", ""));
        if (std::ranges::find(kAllValues, name) == kAllValues.end())
        {
            return fail(ErrorCode::InvalidData, "machine journal: a value not ours");
        }
        journal.values.push_back(
            {name, json::stringOr(entry, "type", "") == "expand", utf8To16Lossy(json::stringOr(entry, "data", ""))});
    }
    return journal;
}

std::wstring storedName(std::string_view sha256)
{
    return utf8To16Lossy(sha256) + L".ico";
}

} // namespace rmw::machine
