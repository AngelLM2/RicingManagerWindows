#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The system tray in our bar (docs/04 M5, ADR-0009): what Shell_NotifyIcon sends to the window of
// class "Shell_TrayWnd" (WM_COPYDATA), the icons it describes, and what a click on one of them
// becomes for its app. Pure: the window, the forwarding to Explorer and the drawing live in rmw-bar.
//
// Everything here arrives from other processes: every size and offset is checked.
namespace rmw::bar::tray
{

// WM_COPYDATA's dwData for the tray window.
inline constexpr std::uintptr_t kNotifyIconData = 1; // Shell_NotifyIcon
inline constexpr std::uintptr_t kIconRectQuery = 3;  // Shell_NotifyIconGetRect

inline constexpr std::uint32_t kSignature = 0x34753423;

enum class Command : std::uint8_t
{
    Add,        // NIM_ADD
    Modify,     // NIM_MODIFY
    Delete,     // NIM_DELETE
    SetFocus,   // NIM_SETFOCUS
    SetVersion, // NIM_SETVERSION
};

using Guid = std::array<std::uint8_t, 16>;

// One Shell_NotifyIcon call, decoded. Window and icon handles are 32-bit on the wire (they are in
// every process: handles of the user's session fit in 32 bits).
struct Update
{
    Command command = Command::Add;
    std::uint32_t owner = 0; // HWND that gets the callbacks
    std::uint32_t id = 0;    // the app's number for the icon
    std::uint32_t flags = 0; // NIF_*
    std::uint32_t callback = 0;
    std::uint32_t icon = 0; // HICON
    std::u16string tip;
    std::uint32_t state = 0;
    std::uint32_t stateMask = 0;
    std::uint32_t version = 0; // NIM_SETVERSION
    std::optional<Guid> guid;  // with NIF_GUID: the icon's identity
};

// NIF_* flags.
inline constexpr std::uint32_t kFlagMessage = 0x01;
inline constexpr std::uint32_t kFlagIcon = 0x02;
inline constexpr std::uint32_t kFlagTip = 0x04;
inline constexpr std::uint32_t kFlagState = 0x08;
inline constexpr std::uint32_t kFlagGuid = 0x20;
inline constexpr std::uint32_t kFlagShowTip = 0x80;
// NIS_* states.
inline constexpr std::uint32_t kStateHidden = 0x01;

// A Shell_NotifyIcon payload (dwData = 1); nullopt when it is not one or is malformed.
[[nodiscard]] std::optional<Update> decodeUpdate(std::span<const std::byte> data);

// Edits a payload decodeUpdate accepted: its NIM_* command, its HICON (what we hand Explorer: our
// copy of the app's icon).
void setCommand(std::span<std::byte> payload, Command command) noexcept;
void setIcon(std::span<std::byte> payload, std::uint32_t icon) noexcept;
// Who the icon is, as text: "guid:<hex>" or "<owner>:<id>".
[[nodiscard]] std::string identity(const Update& update);

// Shell_NotifyIconGetRect (dwData = 3): which icon, and what of its rectangle: shell32 asks for
// the top-left corner, then for the size (measured in the Sandbox: it adds the second answer to
// the first).
struct RectQuery
{
    bool size = false; // false: the top-left corner
    std::uint32_t owner = 0;
    std::uint32_t id = 0;
    std::optional<Guid> guid;
};
[[nodiscard]] std::optional<RectQuery> decodeRectQuery(std::span<const std::byte> data);
// The answer: two coordinates packed like MAKELONG(x, y).
[[nodiscard]] std::int32_t packPoint(int x, int y) noexcept;

struct Icon
{
    std::uint64_t number = 0; // ours, stable while the icon lives: what the bar and plugins use
    std::uint32_t owner = 0;
    std::uint32_t id = 0;
    std::optional<Guid> guid;
    std::uint32_t callback = 0;
    std::uint32_t version = 0;
    std::uint32_t icon = 0;
    std::u16string tip;
    bool showTip = false;
    bool hidden = false;
};

// The icons, in the order apps added them.
class Model
{
public:
    enum class Change : std::uint8_t
    {
        None,
        Added,
        Changed,
        Removed,
    };
    struct Result
    {
        Change change = Change::None;
        std::uint64_t number = 0;
        bool iconChanged = false; // an HICON to copy (again)
    };

    Result apply(const Update& update);
    // Drops icons whose owner window is gone (apps that crash never delete theirs).
    std::vector<std::uint64_t> removeIf(bool (*gone)(std::uint32_t owner));
    void clear() noexcept { m_icons.clear(); }

    [[nodiscard]] const std::vector<Icon>& icons() const noexcept { return m_icons; }
    [[nodiscard]] const Icon* find(std::uint64_t number) const noexcept;
    [[nodiscard]] const Icon* find(std::uint32_t owner, std::uint32_t id,
                                   const std::optional<Guid>& guid) const;

    static constexpr std::size_t kMaxIcons = 256;

private:
    std::vector<Icon>::iterator locate(std::uint32_t owner, std::uint32_t id,
                                       const std::optional<Guid>& guid);

    std::vector<Icon> m_icons;
    std::uint64_t m_next = 1;
};

enum class Gesture : std::uint8_t
{
    LeftClick,
    RightClick,
    DoubleClick,
    MiddleClick,
    HoverStart,
    HoverEnd,
};

// A callback message for the app: PostMessage(owner, icon.callback, wParam, lParam).
struct Post
{
    std::uintptr_t wParam = 0;
    std::intptr_t lParam = 0;

    friend bool operator==(const Post&, const Post&) = default;
};

// What Explorer sends for a gesture, by the icon's version: before 4, wParam is the icon's id and
// lParam the mouse message; from 4 (NOTIFYICON_VERSION_4), wParam holds the anchor point and lParam
// the event with the id in its high word. From 3 a left click adds NIN_SELECT and a right click
// WM_CONTEXTMENU. Hovering a version-4 icon that draws its own tip opens and closes it (NIN_POPUP*).
[[nodiscard]] std::vector<Post> postsFor(const Icon& icon, Gesture gesture, int x, int y);

// Whether the bar shows the standard tip for it (or the app shows its own popup).
[[nodiscard]] bool barShowsTip(const Icon& icon) noexcept;

} // namespace rmw::bar::tray
