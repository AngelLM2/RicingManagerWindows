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
export component RmwStart inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    callback clicked();
    min-width: 36px;
    min-height: 32px;
    preferred-width: 40px;
    preferred-height: 40px;
    border-radius: 6px;
    background: touch.pressed ? rgba(255, 255, 255, 0.20) : touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    // A neutral four-tile mark.
    for i in 4: Rectangle {
        x: (parent.width - 16px) / 2 + mod(i, 2) * 9px;
        y: (parent.height - 16px) / 2 + floor(i / 2) * 9px;
        width: 7px;
        height: 7px;
        border-radius: 1.5px;
        background: root.foreground;
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
    in property <bool> labels;
    callback activate(int);
    min-width: 40px;
    min-height: 32px;
    preferred-width: root.labels ? 180px : 44px;
    max-width: root.labels ? 220px : 48px;
    preferred-height: 40px;
    border-radius: 6px;
    background: root.item.active ? rgba(255, 255, 255, 0.16) : touch.has-hover ? rgba(255, 255, 255, 0.09) : transparent;
    HorizontalLayout {
        padding-left: 10px;
        padding-right: 10px;
        spacing: 8px;
        alignment: root.labels ? LayoutAlignment.start : LayoutAlignment.center;
        VerticalLayout {
            alignment: center;
            Image {
                source: root.item.icon;
                width: 20px;
                height: 20px;
                image-fit: contain;
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
    // The active window's mark.
    Rectangle {
        width: root.item.active ? 16px : 6px;
        height: 3px;
        x: (parent.width - self.width) / 2;
        y: parent.height - 4px;
        border-radius: 1.5px;
        background: root.item.active ? root.foreground : root.foreground.transparentize(60%);
    }
    touch := TouchArea {
        clicked => { root.activate(root.item.id); }
    }
}

export component RmwWindows inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <[RmwWindowItem]> windows;
    in property <bool> labels: true;
    callback activate(int);
    spacing: 2px;
    for item in root.windows: RmwWindowButton {
        item: item;
        foreground: root.foreground;
        labels: root.labels;
        activate(id) => { root.activate(id); }
    }
}

export component RmwWindowsColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <[RmwWindowItem]> windows;
    in property <bool> labels;
    callback activate(int);
    spacing: 2px;
    for item in root.windows: RmwWindowButton {
        item: item;
        foreground: root.foreground;
        labels: false;
        activate(id) => { root.activate(id); }
    }
}

// A short text that updates itself (cpu, memory, network, battery, volume).
export component RmwLabel inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <string> text;
    in property <bool> shown: true;
    callback clicked();
    callback scrolled(int);
    visible: root.shown;
    min-width: root.shown ? label.preferred-width + 16px : 0px;
    min-height: root.shown ? 32px : 0px;
    border-radius: 6px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    label := Text {
        text: root.text;
        color: root.foreground;
        font-size: 12px;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
        scroll-event(event) => {
            root.scrolled(event.delta-y > 0 ? 1 : -1);
            accept
        }
    }
}

// Notifications (or quick settings): a bell from the system icon font.
export component RmwBell inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    callback clicked();
    min-width: 36px;
    min-height: 32px;
    preferred-width: 40px;
    border-radius: 6px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    Text {
        text: "\u{E7E7}";
        font-family: "Segoe Fluent Icons";
        font-size: 16px;
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
    callback clicked();
    width: 28px;
    border-radius: 6px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.14) : transparent;
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
    border-radius: 6px;
    HorizontalLayout {
        padding-left: 8px;
        padding-right: 2px;
        spacing: 2px;
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
            clicked => { root.previous(); }
        }
        RmwGlyph {
            glyph: root.playing ? "\u{E769}" : "\u{E768}";
            foreground: root.foreground;
            clicked => { root.toggle(); }
        }
        RmwGlyph {
            glyph: "\u{E893}";
            foreground: root.foreground;
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
    callback select(int);
    min-width: 26px;
    min-height: 26px;
    preferred-width: 30px;
    preferred-height: 30px;
    border-radius: 6px;
    background: root.desktop.active ? rgba(255, 255, 255, 0.18) : touch.has-hover ? rgba(255, 255, 255, 0.09) : transparent;
    Text {
        text: "\{root.desktop.index + 1}";
        color: root.foreground;
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
    in property <[RmwDesktopItem]> desktops;
    callback select(int);
    spacing: 2px;
    for desktop in root.desktops: RmwDesktopButton {
        desktop: desktop;
        foreground: root.foreground;
        select(index) => { root.select(index); }
    }
}

export component RmwDesktopsColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <[RmwDesktopItem]> desktops;
    callback select(int);
    spacing: 2px;
    for desktop in root.desktops: RmwDesktopButton {
        desktop: desktop;
        foreground: root.foreground;
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
    // kind: 0 left, 1 right, 2 double, 3 hover starts, 4 hover ends, 5 middle (TrayWidget.cpp)
    callback act(int, int);
    min-width: 24px;
    min-height: 24px;
    preferred-width: 28px;
    preferred-height: 32px;
    border-radius: 4px;
    background: touch.pressed ? rgba(255, 255, 255, 0.20) : touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    Image {
        source: root.item.icon;
        width: 16px;
        height: 16px;
        image-fit: contain;
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
    in property <bool> expanded;
    callback toggle();
    min-width: 20px;
    min-height: 24px;
    preferred-width: 22px;
    preferred-height: 32px;
    border-radius: 4px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    Text {
        // Segoe Fluent Icons: chevron up (more) / down (fewer)
        text: root.expanded ? "\u{E70D}" : "\u{E70E}";
        font-family: "Segoe Fluent Icons";
        font-size: 10px;
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
    in property <[RmwTrayItem]> icons;
    in property <int> hidden-count;
    in property <bool> expanded;
    callback act(int, int);
    callback toggle();
    spacing: 0px;
    if root.hidden-count > 0: RmwTrayChevron {
        foreground: root.foreground;
        expanded: root.expanded;
        toggle => { root.toggle(); }
    }
    for item in root.icons: RmwTrayButton {
        item: item;
        act(id, kind) => { root.act(id, kind); }
    }
}

export component RmwTrayColumn inherits VerticalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <[RmwTrayItem]> icons;
    in property <int> hidden-count;
    in property <bool> expanded;
    callback act(int, int);
    callback toggle();
    spacing: 0px;
    if root.hidden-count > 0: RmwTrayChevron {
        foreground: root.foreground;
        expanded: root.expanded;
        toggle => { root.toggle(); }
    }
    for item in root.icons: RmwTrayButton {
        item: item;
        act(id, kind) => { root.act(id, kind); }
    }
}

export component RmwClock inherits Rectangle {
    in property <color> foreground;
    in property <bool> vertical;
    in property <string> text;
    callback clicked();
    min-width: label.preferred-width + 16px;
    min-height: 32px;
    border-radius: 6px;
    background: touch.has-hover ? rgba(255, 255, 255, 0.12) : transparent;
    label := Text {
        text: root.text;
        color: root.foreground;
        font-size: 13px;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}
)slint";

constexpr std::array<bar::WidgetProperty, 1> kTextProperty = {{{"text", "string"}}};
constexpr std::array<bar::WidgetProperty, 2> kWindowsProperties = {
    {{"windows", "[RmwWindowItem]"}, {"labels", "bool"}}};
constexpr std::array<bar::WidgetProperty, 2> kLabelProperties = {{{"text", "string"}, {"shown", "bool"}}};
constexpr std::array<bar::WidgetProperty, 1> kDesktopsProperties = {{{"desktops", "[RmwDesktopItem]"}}};
constexpr std::array<std::string_view, 1> kClicked = {"clicked"};
constexpr std::array<std::string_view, 2> kLabelCallbacks = {"clicked", "scrolled(int)"};
constexpr std::array<std::string_view, 1> kActivate = {"activate(int)"};
constexpr std::array<std::string_view, 1> kSelect = {"select(int)"};
constexpr std::array<bar::WidgetProperty, 3> kMediaProperties = {
    {{"title", "string"}, {"playing", "bool"}, {"shown", "bool"}}};
constexpr std::array<std::string_view, 3> kMediaCallbacks = {"toggle", "next", "previous"};
constexpr std::array<bar::WidgetProperty, 3> kTrayProperties = {
    {{"icons", "[RmwTrayItem]"}, {"hidden-count", "int"}, {"expanded", "bool"}}};
constexpr std::array<std::string_view, 2> kTrayCallbacks = {"act(int, int)", "toggle"};
constexpr std::array<bar::WidgetKind, 12> kKinds = {{
    {"tray", "RmwTray", kTrayProperties, kTrayCallbacks, "RmwTrayColumn"},
    {"media", "RmwMedia", kMediaProperties, kMediaCallbacks},
    {"start", "RmwStart", {}, kClicked},
    {"clock", "RmwClock", kTextProperty, kClicked},
    {"windows", "RmwWindows", kWindowsProperties, kActivate, "RmwWindowsColumn"},
    {"cpu", "RmwLabel", kLabelProperties, kLabelCallbacks},
    {"memory", "RmwLabel", kLabelProperties, kLabelCallbacks},
    {"network", "RmwLabel", kLabelProperties, kLabelCallbacks},
    {"battery", "RmwLabel", kLabelProperties, kLabelCallbacks},
    {"volume", "RmwLabel", kLabelProperties, kLabelCallbacks},
    {"notifications", "RmwBell", {}, kClicked},
    {"desktops", "RmwDesktops", kDesktopsProperties, kSelect, "RmwDesktopsColumn"},
}};

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
        : m_property(slot.prefix + "-text")
        , m_format(settingString(*slot.item, "format", "%H:%M").substr(0, kMaxClockFormat))
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
        if (text == m_shown)
        {
            return;
        }
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_property, Value(slint::SharedString(text)));
        }
        m_shown = std::move(text);
    }

    std::string m_property;
    std::string m_format;
    std::string m_shown;
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
