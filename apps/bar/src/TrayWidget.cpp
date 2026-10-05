#include "TrayWidget.hpp"

#include "TrayHost.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

// The tooltips look like the rest of Windows (comctl32 6).
#pragma comment(                                                                                             \
    linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' "                   \
            "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;
using namespace std::chrono_literals;
namespace tray = bar::tray;

constexpr auto kTipDelay = 450ms;
constexpr int kTipGap = 12;
constexpr int kTipMaxWidth = 400;

// The callback's second argument (RmwTrayButton in the widget sources).
enum ActKind : int
{
    kLeft = 0,
    kRight = 1,
    kDouble = 2,
    kHoverStart = 3,
    kHoverEnd = 4,
    kMiddle = 5,
};

// One standard tooltip for the whole bar, placed by hand: above the cursor on a bar at the bottom
// of the screen, below it otherwise.
class Tooltip
{
public:
    static Tooltip& instance()
    {
        static Tooltip tip;
        return tip;
    }

    bool show(const std::string& text, POINT at)
    {
        if (!ensure())
        {
            return false;
        }
        m_text = utf8To16Lossy(text);
        m_tool.lpszText = m_text.data();
        SendMessageW(m_window, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&m_tool));
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor);
        // Shown far from the cursor first: its size is only known once it is up. Over the cursor
        // it would take the mouse from the bar (which would close it, and open it again...).
        SendMessageW(m_window, TTM_TRACKPOSITION, 0,
                     MAKELPARAM(monitor.rcMonitor.left, monitor.rcMonitor.top));
        SendMessageW(m_window, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&m_tool));
        RECT shown{};
        GetWindowRect(m_window, &shown);
        const int width = shown.right - shown.left;
        const int height = shown.bottom - shown.top;
        const bool lowerHalf = at.y > (monitor.rcMonitor.top + monitor.rcMonitor.bottom) / 2;
        const int x =
            std::clamp(static_cast<int>(at.x) - (width / 2), static_cast<int>(monitor.rcMonitor.left),
                       std::max(static_cast<int>(monitor.rcMonitor.left),
                                static_cast<int>(monitor.rcMonitor.right) - width));
        const int y =
            lowerHalf ? static_cast<int>(at.y) - height - kTipGap : static_cast<int>(at.y) + kTipGap + 8;
        SendMessageW(m_window, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
        return true;
    }

    void hide()
    {
        if (m_window != nullptr)
        {
            SendMessageW(m_window, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&m_tool));
        }
    }

private:
    bool ensure()
    {
        if (m_window != nullptr)
        {
            return true;
        }
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_WIN95_CLASSES;
        InitCommonControlsEx(&controls);
        m_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW,
                                   nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        if (m_window == nullptr)
        {
            log::warn("tray: cannot create the tooltip window ({})", GetLastError());
            return false;
        }
        m_tool.cbSize = sizeof(m_tool);
        m_tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        m_tool.lpszText = m_text.data();
        SendMessageW(m_window, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&m_tool));
        SendMessageW(m_window, TTM_SETMAXTIPWIDTH, 0, kTipMaxWidth); // tips may have several lines
        return true;
    }

    HWND m_window = nullptr;
    TTTOOLINFOW m_tool{};
    std::wstring m_text = L" ";
};

class TrayProvider final : public Provider
{
public:
    TrayProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_instance(instance)
        , m_all(settingString(*slot.item, "show", "promoted") == "all")
    {
        instance->set_callback(m_prefix + "-act", [this](std::span<const Value> arguments) {
            if (arguments.size() == 2)
            {
                const auto index = arguments[0].to_number();
                const auto kind = arguments[1].to_number();
                if (index && kind)
                {
                    act(static_cast<int>(*index), static_cast<int>(*kind));
                }
            }
            return Value();
        });
        instance->set_callback(m_prefix + "-toggle", [this](std::span<const Value>) {
            m_expanded = !m_expanded;
            update();
            return Value();
        });
        m_subscription = TrayHost::instance().subscribe([this] { update(); });
        update();
    }

    ~TrayProvider() override
    {
        Tooltip::instance().hide();
        TrayHost::instance().unsubscribe(m_subscription);
    }
    TrayProvider(const TrayProvider&) = delete;
    TrayProvider& operator=(const TrayProvider&) = delete;
    TrayProvider(TrayProvider&&) = delete;
    TrayProvider& operator=(TrayProvider&&) = delete;

private:
    struct Cached
    {
        std::uint64_t revision = 0;
        slint::Image image;
    };

    // The rows are changed in place while the same icons show: the buttons stay (and so does the
    // mouse's hover on one of them) when an app only changes its icon or tip.
    void update()
    {
        std::vector<std::uint64_t> shown;
        std::vector<Value> rows;
        int overflow = 0;
        std::map<std::uint64_t, Cached> images;
        for (const auto& view : TrayHost::instance().icons())
        {
            if (view.icon.hidden)
            {
                continue;
            }
            if (!m_all && !view.promoted)
            {
                ++overflow;
                if (!m_expanded)
                {
                    continue;
                }
            }
            Cached& cached = images[view.icon.number];
            if (const auto known = m_images.find(view.icon.number);
                known != m_images.end() && known->second.revision == view.revision)
            {
                cached = known->second;
            }
            else
            {
                cached = {view.revision, imageFromPixels(view.pixels)};
            }
            const auto index = static_cast<double>(shown.size());
            shown.push_back(view.icon.number);
            rows.emplace_back(
                slint::interpreter::Struct{{"id", Value(index)}, {"icon", Value(cached.image)}});
        }
        const bool sameIcons = m_model != nullptr && shown == m_shown;
        std::vector<bool> changed(rows.size(), !sameIcons);
        if (sameIcons)
        {
            for (std::size_t i = 0; i < shown.size(); ++i)
            {
                const auto before = m_images.find(shown[i]);
                changed[i] = before == m_images.end() || before->second.revision != images[shown[i]].revision;
            }
        }
        m_images = std::move(images);
        m_shown = std::move(shown);
        if (m_shown != m_logged)
        {
            // Which icon is where (the system tests click by it); numbers only, no tips.
            std::string order;
            for (const std::uint64_t number : m_shown)
            {
                order += order.empty() ? std::to_string(number) : ", " + std::to_string(number);
            }
            log::info("tray: {} shows icons [{}], {} in the overflow", m_prefix, order, overflow);
            m_logged = m_shown;
        }
        auto instance = m_instance.lock();
        if (!instance)
        {
            return;
        }
        if (sameIcons)
        {
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (changed[i])
                {
                    m_model->set_row_data(i, rows[i]);
                }
            }
        }
        else
        {
            m_model = std::make_shared<slint::VectorModel<Value>>(std::move(rows));
            (*instance)->set_property(m_prefix + "-icons",
                                      Value(std::shared_ptr<slint::Model<Value>>(m_model)));
        }
        (*instance)->set_property(m_prefix + "-hidden-count", Value(static_cast<double>(overflow)));
        (*instance)->set_property(m_prefix + "-expanded", Value(m_expanded));
    }

    void act(int index, int kind)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_shown.size())
        {
            return;
        }
        const std::uint64_t number = m_shown[static_cast<std::size_t>(index)];
        POINT cursor{};
        GetCursorPos(&cursor);
        auto& host = TrayHost::instance();
        switch (kind)
        {
        case kLeft: {
            Tooltip::instance().hide();
            // The second click of a double click: the double click itself comes next (Explorer sends
            // down, up, double, up — not two clicks and a double).
            const auto now = std::chrono::steady_clock::now();
            const bool second = m_lastClick == number &&
                                now - m_lastClickAt < std::chrono::milliseconds(GetDoubleClickTime());
            m_lastClick = second ? 0 : number;
            m_lastClickAt = now;
            if (!second)
            {
                host.gesture(number, tray::Gesture::LeftClick, cursor.x, cursor.y);
            }
            break;
        }
        case kRight:
            Tooltip::instance().hide();
            host.gesture(number, tray::Gesture::RightClick, cursor.x, cursor.y);
            break;
        case kDouble:
            host.gesture(number, tray::Gesture::DoubleClick, cursor.x, cursor.y);
            break;
        case kMiddle:
            host.gesture(number, tray::Gesture::MiddleClick, cursor.x, cursor.y);
            break;
        case kHoverStart:
            if (m_hovered == number)
            {
                break; // the same hover, told again
            }
            m_hovered = number;
            host.gesture(number, tray::Gesture::HoverStart, cursor.x, cursor.y);
            m_tipTimer.start(slint::TimerMode::SingleShot, kTipDelay, [this] { showTip(); });
            break;
        case kHoverEnd:
            if (m_hovered == number)
            {
                m_hovered = 0;
                m_tipTimer.stop();
                Tooltip::instance().hide();
            }
            host.gesture(number, tray::Gesture::HoverEnd, cursor.x, cursor.y);
            break;
        default:
            break;
        }
    }

    void showTip()
    {
        const auto* view = TrayHost::instance().find(m_hovered);
        if (view == nullptr || !tray::barShowsTip(view->icon))
        {
            return; // a version-4 icon without NIF_SHOWTIP shows its own popup
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        if (Tooltip::instance().show(view->tip, cursor))
        {
            log::info("tray: tip shown for icon {}", m_hovered);
        }
    }

    std::string m_prefix;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    bool m_all = false;
    bool m_expanded = false;
    std::size_t m_subscription = 0;
    std::shared_ptr<slint::VectorModel<Value>> m_model;
    std::vector<std::uint64_t> m_shown; // index in the model -> icon number
    std::vector<std::uint64_t> m_logged;
    std::map<std::uint64_t, Cached> m_images;
    std::uint64_t m_hovered = 0;
    std::uint64_t m_lastClick = 0;
    std::chrono::steady_clock::time_point m_lastClickAt;
    slint::Timer m_tipTimer; // last: stops before what it uses goes away
};

} // namespace

std::unique_ptr<Provider> makeTrayProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<TrayProvider>(slot, instance);
}

} // namespace rmw::barapp
