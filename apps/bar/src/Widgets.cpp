#include "Widgets.hpp"

#include "MediaWidget.hpp"
#include "SystemWidgets.hpp"
#include "TrayWidget.hpp"
#include "WindowList.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/NativeTaskbar.hpp"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

// Every widget takes `foreground` and `vertical` from the bar (rmw::bar::generateBarSource).
constexpr std::string_view kSources = R"slint(
// ---- built-in widgets (rmw-bar) ------------------------------------------------------------------
// Every one takes from the bar: `foreground`, `vertical`, `item-radius` (its highlights follow the
// bar's corners) and `accent` (Windows' accent color, for what is active). Highlights fade in and
// out; icons come from Segoe Fluent Icons, Windows 11's own icon font.

component RmwHighlight inherits Rectangle {
    in property <color> foreground;
    in property <length> item-radius;
    in property <bool> hovered;
    in property <bool> pressed;
    in property <bool> active;
    border-radius: root.item-radius;
    background: root.pressed ? root.foreground.transparentize(80%)
        : root.active ? root.foreground.transparentize(85%)
        : root.hovered ? root.foreground.transparentize(89%)
        : root.foreground.transparentize(100%);
    animate background { duration: 120ms; easing: ease-out; }
}

export component RmwStart inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    callback clicked();
    min-width: 36px;
    min-height: 32px;
    preferred-width: 42px;
    preferred-height: 40px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    // Four tiles in the accent color; they draw together a little when pressed.
    property <length> tile: touch.pressed ? 6.5px : 7px;
    animate tile { duration: 90ms; easing: ease-out; }
    for i in 4: Rectangle {
        x: (parent.width - 2 * root.tile - 2px) / 2 + mod(i, 2) * (root.tile + 2px);
        y: (parent.height - 2 * root.tile - 2px) / 2 + floor(i / 2) * (root.tile + 2px);
        width: root.tile;
        height: root.tile;
        border-radius: 1.5px;
        background: root.accent;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}

export struct RmwWindowItem {
    id: int,
    title: string,
    icon: image,
    active: bool,
}

component RmwWindowButton inherits Rectangle {
    in property <RmwWindowItem> item;
    in property <color> foreground;
    in property <length> item-radius;
    in property <color> accent;
    in property <bool> labels;
    callback activate(int);
    min-width: 40px;
    min-height: 32px;
    preferred-width: root.labels ? 180px : 44px;
    max-width: root.labels ? 220px : 48px;
    preferred-height: 40px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
        active: root.item.active;
    }
    HorizontalLayout {
        padding-left: 10px;
        padding-right: 10px;
        spacing: 8px;
        alignment: root.labels ? LayoutAlignment.start : LayoutAlignment.center;
        VerticalLayout {
            alignment: center;
            Image {
                source: root.item.icon;
                width: 22px;
                height: 22px;
                image-fit: contain;
                image-rendering: smooth;
            }
        }
        if root.labels: Text {
            text: root.item.title;
            color: root.foreground;
            font-size: 12px;
            overflow: elide;
            vertical-alignment: center;
        }
    }
    // The open window's mark: a dot, a longer accent bar for the active one.
    Rectangle {
        width: root.item.active ? 16px : 6px;
        height: 3px;
        x: (parent.width - self.width) / 2;
        y: parent.height - 4px;
        border-radius: 1.5px;
        background: root.item.active ? root.accent : root.foreground.transparentize(55%);
        animate width, background { duration: 160ms; easing: ease-out; }
    }
    touch := TouchArea {
        clicked => { root.activate(root.item.id); }
    }
}

export component RmwWindows inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwWindowItem]> windows;
    in property <bool> labels: true;
    callback activate(int);
    spacing: 2px;
    for item in root.windows: RmwWindowButton {
        item: item;
        foreground: root.foreground;
        item-radius: root.item-radius;
        accent: root.accent;
        labels: root.labels;
        activate(id) => { root.activate(id); }
    }
}

export component RmwWindowsColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwWindowItem]> windows;
    in property <bool> labels;
    callback activate(int);
    spacing: 2px;
    for item in root.windows: RmwWindowButton {
        item: item;
        foreground: root.foreground;
        item-radius: root.item-radius;
        accent: root.accent;
        labels: false;
        activate(id) => { root.activate(id); }
    }
}

// A status that updates itself (volume, network, battery, cpu, memory): an icon, and a short text
// when the rice wants it.
export component RmwStatus inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <string> text;
    in property <string> glyph;
    in property <bool> show-text: true;
    in property <bool> shown: true;
    callback clicked();
    callback scrolled(int);
    visible: root.shown;
    min-width: root.shown ? max(32px, row.preferred-width) : 0px;
    min-height: root.shown ? 32px : 0px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    row := HorizontalLayout {
        padding-left: 8px;
        padding-right: 8px;
        spacing: 6px;
        alignment: center;
        if root.glyph != "": Text {
            text: root.glyph;
            font-family: "Segoe Fluent Icons";
            font-size: 15px;
            color: root.foreground;
            vertical-alignment: center;
            horizontal-alignment: center;
        }
        if root.show-text && root.text != "": Text {
            text: root.text;
            color: root.foreground;
            font-size: 12px;
            vertical-alignment: center;
        }
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
        scroll-event(event) => {
            root.scrolled(event.delta-y > 0 ? 1 : -1);
            accept
        }
    }
}

// Notifications (or quick settings): a bell.
export component RmwBell inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    callback clicked();
    min-width: 36px;
    min-height: 32px;
    preferred-width: 40px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    Text {
        text: "\u{EA8F}";
        font-family: "Segoe Fluent Icons";
        font-size: 15px;
        color: root.foreground;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}

component RmwGlyph inherits Rectangle {
    in property <string> glyph;
    in property <color> foreground;
    in property <length> item-radius;
    callback clicked();
    width: 28px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    Text {
        text: root.glyph;
        font-family: "Segoe Fluent Icons";
        font-size: 12px;
        color: root.foreground;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}

// What is playing: title and artist, previous / play-pause / next.
export component RmwMedia inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <string> title;
    in property <bool> playing;
    in property <bool> shown;
    callback toggle();
    callback next();
    callback previous();
    visible: root.shown;
    min-width: root.shown ? 140px : 0px;
    preferred-width: root.shown ? 260px : 0px;
    max-width: root.shown ? 320px : 0px;
    min-height: root.shown ? 32px : 0px;
    HorizontalLayout {
        padding-left: 8px;
        padding-right: 2px;
        spacing: 2px;
        Text {
            text: "\u{E8D6}";
            font-family: "Segoe Fluent Icons";
            font-size: 13px;
            color: root.accent;
            vertical-alignment: center;
        }
        Rectangle { width: 4px; }
        Text {
            text: root.title;
            color: root.foreground;
            font-size: 12px;
            overflow: elide;
            vertical-alignment: center;
            horizontal-stretch: 1;
        }
        RmwGlyph {
            glyph: "\u{E892}";
            foreground: root.foreground;
            item-radius: root.item-radius;
            clicked => { root.previous(); }
        }
        RmwGlyph {
            glyph: root.playing ? "\u{E769}" : "\u{E768}";
            foreground: root.foreground;
            item-radius: root.item-radius;
            clicked => { root.toggle(); }
        }
        RmwGlyph {
            glyph: "\u{E893}";
            foreground: root.foreground;
            item-radius: root.item-radius;
            clicked => { root.next(); }
        }
    }
}

export struct RmwDesktopItem {
    index: int,
    active: bool,
}

component RmwDesktopButton inherits Rectangle {
    in property <RmwDesktopItem> desktop;
    in property <color> foreground;
    in property <length> item-radius;
    in property <color> accent;
    callback select(int);
    min-width: 26px;
    min-height: 26px;
    preferred-width: 30px;
    preferred-height: 30px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    // The current one: an accent pill under its number.
    Rectangle {
        width: root.desktop.active ? 14px : 0px;
        height: 3px;
        x: (parent.width - self.width) / 2;
        y: parent.height - 4px;
        border-radius: 1.5px;
        background: root.accent;
        animate width { duration: 160ms; easing: ease-out; }
    }
    Text {
        text: "\{root.desktop.index + 1}";
        color: root.desktop.active ? root.foreground : root.foreground.transparentize(35%);
        font-size: 12px;
        font-weight: root.desktop.active ? 700 : 400;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.select(root.desktop.index); }
    }
}

export component RmwDesktops inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwDesktopItem]> desktops;
    callback select(int);
    spacing: 2px;
    for desktop in root.desktops: RmwDesktopButton {
        desktop: desktop;
        foreground: root.foreground;
        item-radius: root.item-radius;
        accent: root.accent;
        select(index) => { root.select(index); }
    }
}

export component RmwDesktopsColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwDesktopItem]> desktops;
    callback select(int);
    spacing: 2px;
    for desktop in root.desktops: RmwDesktopButton {
        desktop: desktop;
        foreground: root.foreground;
        item-radius: root.item-radius;
        accent: root.accent;
        select(index) => { root.select(index); }
    }
}

// The system tray (M5): the apps' icons; a click goes back to the app (rmw-bar's TrayHost).
export struct RmwTrayItem {
    id: int,
    icon: image,
}

component RmwTrayButton inherits Rectangle {
    in property <RmwTrayItem> item;
    in property <color> foreground;
    in property <length> item-radius;
    // kind: 0 left, 1 right, 2 double, 3 hover starts, 4 hover ends, 5 middle (TrayWidget.cpp)
    callback act(int, int);
    min-width: 24px;
    min-height: 24px;
    preferred-width: 30px;
    preferred-height: 32px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: min(root.item-radius, 6px);
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    Image {
        source: root.item.icon;
        width: 16px;
        height: 16px;
        image-fit: contain;
        image-rendering: smooth;
    }
    touch := TouchArea {
        clicked => { root.act(root.item.id, 0); }
        double-clicked => { root.act(root.item.id, 2); }
        pointer-event(event) => {
            if (event.kind == PointerEventKind.up && event.button == PointerEventButton.right) {
                root.act(root.item.id, 1);
            }
            if (event.kind == PointerEventKind.up && event.button == PointerEventButton.middle) {
                root.act(root.item.id, 5);
            }
        }
        changed has-hover => { root.act(root.item.id, self.has-hover ? 3 : 4); }
    }
}

component RmwTrayChevron inherits Rectangle {
    in property <color> foreground;
    in property <length> item-radius;
    in property <bool> expanded;
    callback toggle();
    min-width: 22px;
    min-height: 24px;
    preferred-width: 26px;
    preferred-height: 32px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: min(root.item-radius, 6px);
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    Text {
        // Segoe Fluent Icons: chevron up (more) / down (fewer)
        text: root.expanded ? "\u{E70D}" : "\u{E70E}";
        font-family: "Segoe Fluent Icons";
        font-size: 11px;
        color: root.foreground;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.toggle(); }
    }
}

export component RmwTray inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwTrayItem]> icons;
    in property <int> hidden-count;
    in property <bool> expanded;
    callback act(int, int);
    callback toggle();
    spacing: 0px;
    if root.hidden-count > 0: RmwTrayChevron {
        foreground: root.foreground;
        item-radius: root.item-radius;
        expanded: root.expanded;
        toggle => { root.toggle(); }
    }
    for item in root.icons: RmwTrayButton {
        item: item;
        foreground: root.foreground;
        item-radius: root.item-radius;
        act(id, kind) => { root.act(id, kind); }
    }
}

export component RmwTrayColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <[RmwTrayItem]> icons;
    in property <int> hidden-count;
    in property <bool> expanded;
    callback act(int, int);
    callback toggle();
    spacing: 0px;
    if root.hidden-count > 0: RmwTrayChevron {
        foreground: root.foreground;
        item-radius: root.item-radius;
        expanded: root.expanded;
        toggle => { root.toggle(); }
    }
    for item in root.icons: RmwTrayButton {
        item: item;
        foreground: root.foreground;
        item-radius: root.item-radius;
        act(id, kind) => { root.act(id, kind); }
    }
}

// The time, and the date under it when the rice asks (`date = "%d/%m/%Y"`).
export component RmwClock inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <length> item-radius: 6px;
    in property <color> accent: #60cdff;
    in property <string> text;
    in property <string> date;
    callback clicked();
    min-width: max(label.preferred-width, day.preferred-width) + 18px;
    min-height: 32px;
    RmwHighlight {
        foreground: root.foreground;
        item-radius: root.item-radius;
        hovered: touch.has-hover;
        pressed: touch.pressed;
    }
    VerticalLayout {
        alignment: center;
        spacing: 0px;
        label := Text {
            text: root.text;
            color: root.foreground;
            font-size: root.date != "" ? 12px : 13px;
            horizontal-alignment: center;
        }
        day := Text {
            visible: root.date != "";
            height: root.date != "" ? self.preferred-height : 0px;
            text: root.date;
            color: root.foreground.transparentize(25%);
            font-size: 11px;
            horizontal-alignment: center;
        }
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}
)slint";

constexpr std::array<bar::WidgetProperty, 2> kClockProperties = {{{"text", "string"}, {"date", "string"}}};
constexpr std::array<bar::WidgetProperty, 2> kWindowsProperties = {
    {{"windows", "[RmwWindowItem]"}, {"labels", "bool"}}};
constexpr std::array<bar::WidgetProperty, 4> kStatusProperties = {
    {{"text", "string"}, {"glyph", "string"}, {"show-text", "bool"}, {"shown", "bool"}}};
constexpr std::array<bar::WidgetProperty, 1> kDesktopsProperties = {{{"desktops", "[RmwDesktopItem]"}}};
constexpr std::array<std::string_view, 1> kClicked = {"clicked"};
constexpr std::array<std::string_view, 2> kStatusCallbacks = {"clicked", "scrolled(int)"};
constexpr std::array<std::string_view, 1> kActivate = {"activate(int)"};
constexpr std::array<std::string_view, 1> kSelect = {"select(int)"};
constexpr std::array<bar::WidgetProperty, 3> kMediaProperties = {
    {{"title", "string"}, {"playing", "bool"}, {"shown", "bool"}}};
constexpr std::array<std::string_view, 3> kMediaCallbacks = {"toggle", "next", "previous"};
constexpr std::array<bar::WidgetProperty, 3> kTrayProperties = {
    {{"icons", "[RmwTrayItem]"}, {"hidden-count", "int"}, {"expanded", "bool"}}};
constexpr std::array<std::string_view, 2> kTrayCallbacks = {"act(int, int)", "toggle"};

// A built-in widget: it takes everything the bar hands out.
constexpr bar::WidgetKind builtin(std::string_view id, std::string_view component,
                                  std::span<const bar::WidgetProperty> properties,
                                  std::span<const std::string_view> callbacks, std::string_view vertical = {})
{
    return {.id = id,
            .component = component,
            .properties = properties,
            .callbacks = callbacks,
            .verticalComponent = vertical,
            .takesForeground = true,
            .takesVertical = true,
            .takesItemRadius = true,
            .takesAccent = true};
}

constexpr std::array<bar::WidgetKind, 12> kKinds = {
    builtin("tray", "RmwTray", kTrayProperties, kTrayCallbacks, "RmwTrayColumn"),
    builtin("media", "RmwMedia", kMediaProperties, kMediaCallbacks),
    builtin("start", "RmwStart", {}, kClicked),
    builtin("clock", "RmwClock", kClockProperties, kClicked),
    builtin("windows", "RmwWindows", kWindowsProperties, kActivate, "RmwWindowsColumn"),
    builtin("cpu", "RmwStatus", kStatusProperties, kStatusCallbacks),
    builtin("memory", "RmwStatus", kStatusProperties, kStatusCallbacks),
    builtin("network", "RmwStatus", kStatusProperties, kStatusCallbacks),
    builtin("battery", "RmwStatus", kStatusProperties, kStatusCallbacks),
    builtin("volume", "RmwStatus", kStatusProperties, kStatusCallbacks),
    builtin("notifications", "RmwBell", {}, kClicked),
    builtin("desktops", "RmwDesktops", kDesktopsProperties, kSelect, "RmwDesktopsColumn"),
};

constexpr std::size_t kMaxClockFormat = 64;

std::string localized(const SYSTEMTIME& time, const wchar_t* picture)
{
    std::array<wchar_t, 64> text{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, picture, text.data(),
                        static_cast<int>(text.size()), nullptr) == 0)
    {
        return {};
    }
    return utf16To8Lossy(text.data());
}

class StartProvider final : public Provider
{
public:
    StartProvider(const bar::Slot& slot, const Instance& instance)
    {
        instance->set_callback(slot.prefix + "-clicked", [](std::span<const Value>) {
            // Opens Start; works with the native taskbar hidden (ADR-0003).
            if (auto* const tray = static_cast<HWND>(platform::nativeTrayWindow()))
            {
                PostMessageW(tray, WM_SYSCOMMAND, SC_TASKLIST, 0);
            }
            return Value();
        });
    }
};

class ClockProvider final : public Provider
{
public:
    ClockProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_format(settingString(*slot.item, "format", "%H:%M").substr(0, kMaxClockFormat))
        , m_dateFormat(settingString(*slot.item, "date", "").substr(0, kMaxClockFormat))
        , m_instance(instance)
    {
        instance->set_callback(slot.prefix + "-clicked", [](std::span<const Value>) {
            // Calendar and notifications.
            ShellExecuteW(nullptr, L"open", L"ms-actioncenter:", nullptr, nullptr, SW_SHOWNORMAL);
            return Value();
        });
        update();
        m_timer.start(slint::TimerMode::Repeated, std::chrono::seconds(1), [this] { update(); });
    }

private:
    void update()
    {
        std::string text = formatClock(m_format);
        std::string date = m_dateFormat.empty() ? std::string() : formatClock(m_dateFormat);
        if (text == m_shown && date == m_shownDate)
        {
            return;
        }
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-text", Value(slint::SharedString(text)));
            (*instance)->set_property(m_prefix + "-date", Value(slint::SharedString(date)));
        }
        m_shown = std::move(text);
        m_shownDate = std::move(date);
    }

    std::string m_prefix;
    std::string m_format;
    std::string m_dateFormat;
    std::string m_shown;
    std::string m_shownDate;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    slint::Timer m_timer; // last: stops before what it uses goes away
};

} // namespace

std::span<const bar::WidgetKind> builtinKinds() noexcept
{
    return kKinds;
}

std::string_view builtinSources() noexcept
{
    return kSources;
}

std::string settingString(const domain::BarItem& item, std::string_view key, std::string_view fallback)
{
    const domain::Value* value = item.config.find(key);
    const auto* text = value != nullptr ? std::get_if<std::string>(&value->data) : nullptr;
    return text != nullptr ? *text : std::string(fallback);
}

bool settingBool(const domain::BarItem& item, std::string_view key, bool fallback)
{
    const domain::Value* value = item.config.find(key);
    const auto* flag = value != nullptr ? std::get_if<bool>(&value->data) : nullptr;
    return flag != nullptr ? *flag : fallback;
}

slint::Image imageFromPixels(const providers::IconPixels& pixels)
{
    if (pixels.empty() || pixels.width <= 0 || pixels.height <= 0)
    {
        return {};
    }
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(static_cast<std::uint32_t>(pixels.width),
                                                       static_cast<std::uint32_t>(pixels.height));
    auto* out = buffer.begin();
    const std::size_t count = std::min(pixels.rgba.size() / 4, static_cast<std::size_t>(pixels.width) *
                                                                   static_cast<std::size_t>(pixels.height));
    for (std::size_t i = 0; i < count; ++i)
    {
        out[i] = slint::Rgba8Pixel{pixels.rgba[(i * 4) + 0], pixels.rgba[(i * 4) + 1],
                                   pixels.rgba[(i * 4) + 2], pixels.rgba[(i * 4) + 3]};
    }
    return {buffer};
}

std::unique_ptr<Provider> makeProvider(const bar::Slot& slot, const Instance& instance)
{
    if (auto system = makeSystemProvider(slot, instance))
    {
        return system;
    }
    if (slot.kind->id == "start")
    {
        return std::make_unique<StartProvider>(slot, instance);
    }
    if (slot.kind->id == "clock")
    {
        return std::make_unique<ClockProvider>(slot, instance);
    }
    if (slot.kind->id == "windows")
    {
        return makeWindowListProvider(slot, instance);
    }
    if (slot.kind->id == "media")
    {
        return makeMediaProvider(slot, instance);
    }
    if (slot.kind->id == "tray")
    {
        return makeTrayProvider(slot, instance);
    }
    return nullptr;
}

std::string formatClock(std::string_view format)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::string text;
    for (std::size_t i = 0; i < format.size(); ++i)
    {
        if (format[i] != '%' || i + 1 == format.size())
        {
            text += format[i];
            continue;
        }
        const char conversion = format[++i];
        switch (conversion)
        {
        case 'H':
            text += std::format("{:02}", now.wHour);
            break;
        case 'I':
            text += std::format("{:02}", now.wHour % 12 == 0 ? 12 : now.wHour % 12);
            break;
        case 'M':
            text += std::format("{:02}", now.wMinute);
            break;
        case 'S':
            text += std::format("{:02}", now.wSecond);
            break;
        case 'p':
            text += now.wHour < 12 ? "AM" : "PM";
            break;
        case 'd':
            text += std::format("{:02}", now.wDay);
            break;
        case 'm':
            text += std::format("{:02}", now.wMonth);
            break;
        case 'y':
            text += std::format("{:02}", now.wYear % 100);
            break;
        case 'Y':
            text += std::format("{}", now.wYear);
            break;
        case 'a':
            text += localized(now, L"ddd");
            break;
        case 'A':
            text += localized(now, L"dddd");
            break;
        case 'b':
            text += localized(now, L"MMM");
            break;
        case 'B':
            text += localized(now, L"MMMM");
            break;
        case '%':
            text += '%';
            break;
        default:
            text += '%';
            text += conversion;
            break;
        }
    }
    return text;
}

} // namespace rmw::barapp
