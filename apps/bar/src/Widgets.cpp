#include "Widgets.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <shellapi.h>

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
constexpr std::array<std::string_view, 1> kClicked = {"clicked"};
constexpr std::array<bar::WidgetKind, 2> kKinds = {{
    {"start", "RmwStart", {}, kClicked},
    {"clock", "RmwClock", kTextProperty, kClicked},
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

std::string stringSetting(const domain::BarItem& item, std::string_view key, std::string_view fallback)
{
    const domain::Value* value = item.config.find(key);
    const auto* text = value != nullptr ? std::get_if<std::string>(&value->data) : nullptr;
    return text != nullptr ? *text : std::string(fallback);
}

class StartProvider final : public Provider
{
public:
    StartProvider(const bar::Slot& slot, const Instance& instance)
    {
        instance->set_callback(slot.prefix + "-clicked", [](std::span<const Value>) {
            // Opens Start; works with the native taskbar hidden (ADR-0003).
            if (auto* const tray = FindWindowW(L"Shell_TrayWnd", nullptr))
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
        , m_format(stringSetting(*slot.item, "format", "%H:%M").substr(0, kMaxClockFormat))
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

std::unique_ptr<Provider> makeProvider(const bar::Slot& slot, const Instance& instance)
{
    if (slot.kind->id == "start")
    {
        return std::make_unique<StartProvider>(slot, instance);
    }
    if (slot.kind->id == "clock")
    {
        return std::make_unique<ClockProvider>(slot, instance);
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
