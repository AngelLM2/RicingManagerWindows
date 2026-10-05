#include "rmw/bar/Tray.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace rmw::bar::tray
{
namespace
{

// The layout shell32 sends (NOTIFYICONDATA with 32-bit handles, packed to 4 bytes), after a
// signature and the NIM_* message.
constexpr std::size_t kMessageOffset = 4;
constexpr std::size_t kOwnerOffset = 12;
constexpr std::size_t kIdOffset = 16;
constexpr std::size_t kFlagsOffset = 20;
constexpr std::size_t kCallbackOffset = 24;
constexpr std::size_t kIconOffset = 28;
constexpr std::size_t kTipOffset = 32;
constexpr std::size_t kTipChars = 128;
constexpr std::size_t kStateOffset = 288;
constexpr std::size_t kStateMaskOffset = 292;
constexpr std::size_t kVersionOffset = 808;
constexpr std::size_t kGuidOffset = 944;
constexpr std::size_t kMinimumUpdate = kStateOffset; // up to the end of the tip

// Shell_NotifyIconGetRect's request: signature, which corner, the caller's cbSize, then the icon.
// Windows 10 and 11 put 4 bytes of padding before the window (40 bytes); without them it is 36.
constexpr std::size_t kRectQueryPadded = 40;
constexpr std::size_t kRectQueryPacked = 36;

// Callback events.
constexpr std::uint32_t kMouseMove = 0x0200;
constexpr std::uint32_t kLeftDown = 0x0201;
constexpr std::uint32_t kLeftUp = 0x0202;
constexpr std::uint32_t kLeftDouble = 0x0203;
constexpr std::uint32_t kRightDown = 0x0204;
constexpr std::uint32_t kRightUp = 0x0205;
constexpr std::uint32_t kMiddleDown = 0x0207;
constexpr std::uint32_t kMiddleUp = 0x0208;
constexpr std::uint32_t kContextMenu = 0x007B;
constexpr std::uint32_t kSelect = 0x0400;     // NIN_SELECT (WM_USER)
constexpr std::uint32_t kPopupOpen = 0x0406;  // NIN_POPUPOPEN
constexpr std::uint32_t kPopupClose = 0x0407; // NIN_POPUPCLOSE

constexpr std::uint32_t kVersion3 = 3; // NOTIFYICON_VERSION
constexpr std::uint32_t kVersion4 = 4; // NOTIFYICON_VERSION_4

std::uint32_t read32(std::span<const std::byte> data, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

Guid readGuid(std::span<const std::byte> data, std::size_t offset)
{
    Guid guid{};
    std::memcpy(guid.data(), data.data() + offset, guid.size());
    return guid;
}

std::u16string readTip(std::span<const std::byte> data)
{
    std::u16string tip;
    for (std::size_t i = 0; i < kTipChars; ++i)
    {
        char16_t c = 0;
        std::memcpy(&c, data.data() + kTipOffset + (i * sizeof(char16_t)), sizeof(c));
        if (c == 0)
        {
            break;
        }
        tip.push_back(c);
    }
    return tip;
}

std::uintptr_t packWord(std::uint32_t low, std::uint32_t high) noexcept
{
    return static_cast<std::uintptr_t>(((high & 0xFFFFU) << 16U) | (low & 0xFFFFU));
}

} // namespace

std::optional<Update> decodeUpdate(std::span<const std::byte> data)
{
    if (data.size() < kMinimumUpdate || read32(data, 0) != kSignature)
    {
        return std::nullopt;
    }
    const std::uint32_t message = read32(data, kMessageOffset);
    if (message > static_cast<std::uint32_t>(Command::SetVersion))
    {
        return std::nullopt;
    }
    Update update;
    update.command = static_cast<Command>(message);
    update.owner = read32(data, kOwnerOffset);
    update.id = read32(data, kIdOffset);
    update.flags = read32(data, kFlagsOffset);
    update.callback = read32(data, kCallbackOffset);
    update.icon = read32(data, kIconOffset);
    update.tip = readTip(data);
    if (data.size() >= kStateMaskOffset + 4)
    {
        update.state = read32(data, kStateOffset);
        update.stateMask = read32(data, kStateMaskOffset);
    }
    if (data.size() >= kVersionOffset + 4)
    {
        update.version = read32(data, kVersionOffset);
    }
    if ((update.flags & kFlagGuid) != 0 && data.size() >= kGuidOffset + sizeof(Guid))
    {
        update.guid = readGuid(data, kGuidOffset);
    }
    return update;
}

void setCommand(std::span<std::byte> payload, Command command) noexcept
{
    if (payload.size() >= kMinimumUpdate)
    {
        const auto value = static_cast<std::uint32_t>(command);
        std::memcpy(payload.data() + kMessageOffset, &value, sizeof(value));
    }
}

void setIcon(std::span<std::byte> payload, std::uint32_t icon) noexcept
{
    if (payload.size() >= kMinimumUpdate)
    {
        std::memcpy(payload.data() + kIconOffset, &icon, sizeof(icon));
    }
}

std::string identity(const Update& update)
{
    if (update.guid)
    {
        std::string text = "guid:";
        for (const std::uint8_t byte : *update.guid)
        {
            constexpr std::string_view kHex = "0123456789abcdef";
            text += kHex[byte >> 4U];
            text += kHex[byte & 0x0FU];
        }
        return text;
    }
    return std::to_string(update.owner) + ":" + std::to_string(update.id);
}

std::optional<RectQuery> decodeRectQuery(std::span<const std::byte> data)
{
    std::size_t owner = 0;
    if (data.size() >= kRectQueryPadded)
    {
        owner = 16;
    }
    else if (data.size() >= kRectQueryPacked)
    {
        owner = 12;
    }
    else
    {
        return std::nullopt;
    }
    if (read32(data, 0) != kSignature)
    {
        return std::nullopt;
    }
    const std::uint32_t message = read32(data, kMessageOffset);
    if (message != 1 && message != 2)
    {
        return std::nullopt;
    }
    RectQuery query;
    query.size = message == 2;
    query.owner = read32(data, owner);
    query.id = read32(data, owner + 4);
    const Guid guid = readGuid(data, owner + 8);
    if (std::ranges::any_of(guid, [](std::uint8_t b) { return b != 0; }))
    {
        query.guid = guid;
    }
    return query;
}

std::int32_t packPoint(int x, int y) noexcept
{
    return static_cast<std::int32_t>(packWord(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
}

std::vector<Icon>::iterator Model::locate(std::uint32_t owner, std::uint32_t id,
                                          const std::optional<Guid>& guid)
{
    return std::ranges::find_if(m_icons, [&](const Icon& icon) {
        if (guid)
        {
            return icon.guid == guid;
        }
        return !icon.guid && icon.owner == owner && icon.id == id;
    });
}

const Icon* Model::find(std::uint64_t number) const noexcept
{
    const auto found = std::ranges::find(m_icons, number, &Icon::number);
    return found == m_icons.end() ? nullptr : &*found;
}

const Icon* Model::find(std::uint32_t owner, std::uint32_t id, const std::optional<Guid>& guid) const
{
    const auto found = std::ranges::find_if(m_icons, [&](const Icon& icon) {
        if (guid && icon.guid)
        {
            return icon.guid == guid;
        }
        return icon.owner == owner && icon.id == id;
    });
    return found == m_icons.end() ? nullptr : &*found;
}

Model::Result Model::apply(const Update& update)
{
    auto found = locate(update.owner, update.id, update.guid);
    if (update.command == Command::Delete)
    {
        if (found == m_icons.end())
        {
            return {};
        }
        const std::uint64_t number = found->number;
        m_icons.erase(found);
        return {Change::Removed, number, false};
    }
    if (update.command == Command::SetVersion)
    {
        if (found == m_icons.end() || found->version == update.version)
        {
            return {};
        }
        found->version = update.version;
        return {Change::Changed, found->number, false};
    }
    if (update.command == Command::SetFocus)
    {
        return {};
    }

    Change change = Change::Changed;
    if (found == m_icons.end())
    {
        // A modify for an icon added before we were there: enough to draw it only with its icon.
        const bool drawable = (update.flags & kFlagIcon) != 0;
        if ((update.command == Command::Modify && !drawable) || m_icons.size() >= kMaxIcons)
        {
            return {};
        }
        Icon icon;
        icon.number = m_next++;
        icon.owner = update.owner;
        icon.id = update.id;
        icon.guid = update.guid;
        m_icons.push_back(icon);
        found = std::prev(m_icons.end());
        change = Change::Added;
    }
    Icon& icon = *found;
    // An icon known by its GUID may come from another window now.
    icon.owner = update.owner;
    icon.id = update.id;
    if ((update.flags & kFlagMessage) != 0)
    {
        icon.callback = update.callback;
    }
    if ((update.flags & kFlagIcon) != 0)
    {
        icon.icon = update.icon;
    }
    if ((update.flags & kFlagTip) != 0)
    {
        icon.tip = update.tip;
    }
    if (update.command == Command::Add || (update.flags & kFlagTip) != 0)
    {
        icon.showTip = (update.flags & kFlagShowTip) != 0;
    }
    if ((update.flags & kFlagState) != 0 && (update.stateMask & kStateHidden) != 0)
    {
        icon.hidden = (update.state & kStateHidden) != 0;
    }
    // A handle may be reused for a new picture: any NIF_ICON is copied again.
    return {change, icon.number, (update.flags & kFlagIcon) != 0};
}

std::vector<std::uint64_t> Model::removeIf(bool (*gone)(std::uint32_t owner))
{
    std::vector<std::uint64_t> removed;
    std::erase_if(m_icons, [&](const Icon& icon) {
        if (gone(icon.owner))
        {
            removed.push_back(icon.number);
            return true;
        }
        return false;
    });
    return removed;
}

std::vector<Post> postsFor(const Icon& icon, Gesture gesture, int x, int y)
{
    if (icon.callback == 0)
    {
        return {};
    }
    const bool four = icon.version >= kVersion4;
    const bool three = icon.version >= kVersion3;
    std::vector<std::uint32_t> events;
    switch (gesture)
    {
    case Gesture::LeftClick:
        events = {kLeftDown, kLeftUp};
        if (three)
        {
            events.push_back(kSelect);
        }
        break;
    case Gesture::RightClick:
        events = {kRightDown, kRightUp};
        if (three)
        {
            events.push_back(kContextMenu);
        }
        break;
    case Gesture::DoubleClick:
        events = {kLeftDouble, kLeftUp};
        break;
    case Gesture::MiddleClick:
        events = {kMiddleDown, kMiddleUp};
        break;
    case Gesture::HoverStart:
        events = {kMouseMove};
        if (four && !icon.showTip)
        {
            events.push_back(kPopupOpen);
        }
        break;
    case Gesture::HoverEnd:
        if (four && !icon.showTip)
        {
            events.push_back(kPopupClose);
        }
        break;
    }
    std::vector<Post> posts;
    posts.reserve(events.size());
    for (const std::uint32_t event : events)
    {
        if (four)
        {
            posts.push_back({packWord(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)),
                             static_cast<std::intptr_t>(packWord(event, icon.id))});
        }
        else
        {
            posts.push_back({icon.id, static_cast<std::intptr_t>(event)});
        }
    }
    return posts;
}

bool barShowsTip(const Icon& icon) noexcept
{
    return !icon.tip.empty() && (icon.version < kVersion4 || icon.showTip);
}

} // namespace rmw::bar::tray
