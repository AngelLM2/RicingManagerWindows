#pragma once

#include "rmw/base/Error.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::ports
{

// A shortcut of the user's: in their Start menu, on their desktop or pinned to Windows' taskbar.
struct Shortcut
{
    std::string path;    // the .lnk
    std::string program; // its target's file name, lower case ("firefox.exe"); empty: not a program
    std::string icon;    // its icon ("C:\x.ico,0"); empty: the target's own

    friend bool operator==(const Shortcut&, const Shortcut&) = default;
};

// The icons Windows shows that live outside the registry (M14): the user's folders (their
// desktop.ini) and the user's own shortcuts. Never the machine's (all users').
class IIcons
{
public:
    virtual ~IIcons() = default;

    // A user folder by its rice name ("documents"): the icon its desktop.ini gives ("C:\x.ico,0"), or
    // nullopt when it gives none.
    [[nodiscard]] virtual Result<std::optional<std::string>> folderIcon(std::string_view folder) = 0;
    // nullopt: the folder's own icon again.
    [[nodiscard]] virtual Result<void> setFolderIcon(std::string_view folder,
                                                     const std::optional<std::string>& icon) = 0;

    [[nodiscard]] virtual Result<std::vector<Shortcut>> shortcuts() = 0;
    // "": the target's own icon again.
    [[nodiscard]] virtual Result<void> setShortcutIcon(const std::string& path, const std::string& icon) = 0;

    // The ProgID Windows uses for a file type ("txt" -> "txtfile"): the user's choice, else the
    // machine's; "" when the type has none.
    [[nodiscard]] virtual Result<std::string> progIdOf(std::string_view extension) = 0;

    // Whether the file an icon location names is there ("%SystemRoot%\x.dll,-5": variables expanded,
    // the index left out).
    [[nodiscard]] virtual bool exists(std::string_view location) = 0;

    // Explorer drops what it remembers of icons and draws them again.
    virtual void refresh() = 0;
};

} // namespace rmw::ports
