#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The machine-wide icons (M14 advanced mode, ADR-0021, doc 10 §4.1): what rmw-elevated is asked to
// do and what it keeps to undo it. Pure: rmw-elevated does the Windows part, as administrator.
namespace rmw::machine
{

// HKEY_LOCAL_MACHINE, 64-bit view.
inline constexpr std::wstring_view kShellIconsKey = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Shell Icons)";

// The Shell Icons values an icon sets: "3" a closed folder and "4" an open one; "8" a fixed drive.
[[nodiscard]] std::span<const std::wstring_view> shellIconValues(domain::MachineIcon icon) noexcept;
// Every value any icon sets (what the journal keeps).
[[nodiscard]] std::span<const std::wstring_view> allShellIconValues() noexcept;

// One icon to set: the .ico's SHA-256 (what rmw-elevated checks the bytes it reads against) and the
// file to read it from (the rice's own; rmw-elevated reads it once, from a handle, and copies those
// bytes).
struct IconRequest
{
    domain::MachineIcon icon = domain::MachineIcon::Folder;
    std::string sha256; // 64 lower-case hex digits
    std::wstring source; // absolute

    friend bool operator==(const IconRequest&, const IconRequest&) = default;
};

// rmw-elevated's whole command line (after the program), a closed list:
//   set-shell-icons [--folder <sha256> <file>] [--drive <sha256> <file>]
//   restore-machine-defaults
// Nothing in it ever becomes a registry key or a destination path.
struct Command
{
    enum class Kind : std::uint8_t
    {
        SetShellIcons,
        RestoreDefaults,
    };
    Kind kind = Kind::RestoreDefaults;
    std::vector<IconRequest> icons;

    friend bool operator==(const Command&, const Command&) = default;
};

[[nodiscard]] Result<Command> parseCommand(std::span<const std::wstring> args);
// The arguments for ShellExecuteEx, quoted; parseCommand(split) gives `command` back.
[[nodiscard]] Result<std::wstring> commandLine(const Command& command);

// What there was before the first change, so that restore-machine-defaults puts it back exactly.
struct SavedValue
{
    std::wstring name;    // "3"
    bool expand = false;  // REG_EXPAND_SZ (else REG_SZ)
    std::wstring data;

    friend bool operator==(const SavedValue&, const SavedValue&) = default;
};

struct Journal
{
    bool keyExisted = false;
    std::vector<SavedValue> values; // the ones that were there; the others were absent

    friend bool operator==(const Journal&, const Journal&) = default;
};

[[nodiscard]] std::string toJson(const Journal& journal);
[[nodiscard]] Result<Journal> parseJournal(std::string_view text);

// "<sha256>.ico": the name an icon gets in rmw-elevated's own folder.
[[nodiscard]] std::wstring storedName(std::string_view sha256);

} // namespace rmw::machine
