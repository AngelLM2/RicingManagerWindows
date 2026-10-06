#include "WindowList.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/providers/Windows.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <utility>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;
using namespace std::chrono_literals;

constexpr int kIconSize = 32;    // a window's own icon: drawn at 22 logical px
constexpr int kAppIconSize = 48; // the app's, from the Shell: sharp up to 200% DPI
constexpr std::size_t kAppIconsKept = 256;
constexpr auto kSettle = 60ms; // events come in bursts: look once they stop
constexpr UINT kIconTimeoutMs = 50;
// Asking windows for their icons costs up to 3 x 50 ms each: at most this much per pass (the UI
// thread's heartbeat is watched), the rest on the next pass.
constexpr auto kIconBudget = 250ms;

std::atomic<WindowTracker*> g_tracker{nullptr}; // WinEvent callbacks carry no context

HICON iconHandle(HWND window)
{
    for (const WPARAM kind : {ICON_SMALL2, ICON_SMALL, ICON_BIG})
    {
        DWORD_PTR result = 0;
        // A hung window must not hang the bar.
        if (SendMessageTimeoutW(window, WM_GETICON, kind, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kIconTimeoutMs,
                                &result) != 0 &&
            result != 0)
        {
            return reinterpret_cast<HICON>(result); // NOLINT(performance-no-int-to-ptr): WM_GETICON
        }
    }
    if (const ULONG_PTR classIcon = GetClassLongPtrW(window, GCLP_HICONSM); classIcon != 0)
    {
        return reinterpret_cast<HICON>(classIcon); // NOLINT(performance-no-int-to-ptr): class icon
    }
    return reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON)); // NOLINT(performance-no-int-to-ptr)
}

// The icon at kIconSize pixels, for Slint.
slint::Image imageOf(HICON icon)
{
    return imageFromPixels(providers::iconPixels(icon, kIconSize));
}

class WindowListProvider final : public Provider
{
public:
    WindowListProvider(const bar::Slot& slot, const Instance& instance)
        : m_property(slot.prefix + "-windows")
        , m_instance(instance)
        , m_tracker(&WindowTracker::instance())
    {
        instance->set_property(slot.prefix + "-labels", Value(settingBool(*slot.item, "labels", true)));
        instance->set_callback(slot.prefix + "-activate", [this](std::span<const Value> arguments) {
            if (!arguments.empty())
            {
                if (const auto number = arguments[0].to_number();
                    number && *number >= 0 && *number < static_cast<double>(m_shown.size()))
                {
                    WindowTracker::activate(m_shown[static_cast<std::size_t>(*number)]);
                }
            }
            return Value();
        });
        m_subscription = m_tracker->subscribe([this] { update(); });
        update();
    }

    ~WindowListProvider() override { m_tracker->unsubscribe(m_subscription); }
    WindowListProvider(const WindowListProvider&) = delete;
    WindowListProvider& operator=(const WindowListProvider&) = delete;
    WindowListProvider(WindowListProvider&&) = delete;
    WindowListProvider& operator=(WindowListProvider&&) = delete;

private:
    void update()
    {
        auto model = std::make_shared<slint::VectorModel<Value>>();
        m_shown.clear();
        for (const auto& entry : m_tracker->windows())
        {
            const auto id = static_cast<double>(m_shown.size());
            m_shown.push_back(entry.window);
            const slint::interpreter::Struct item{
                {"id", Value(id)},
                {"title", Value(slint::SharedString(entry.title))},
                {"icon", Value(entry.icon)},
                {"active", Value(entry.window == m_tracker->foreground())},
            };
            model->push_back(Value(item));
        }
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_property, Value(std::shared_ptr<slint::Model<Value>>(model)));
        }
    }

    std::string m_property;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    WindowTracker* m_tracker;
    std::size_t m_subscription = 0;
    std::vector<void*> m_shown; // index in the model -> HWND
};

} // namespace

struct WindowTracker::Access
{
    static void changed()
    {
        if (auto* tracker = g_tracker.load())
        {
            tracker->schedule();
        }
    }
};

WindowTracker& WindowTracker::instance()
{
    static WindowTracker tracker;
    return tracker;
}

WindowTracker::WindowTracker()
{
    g_tracker = this;
    m_hooks = std::make_unique<providers::WindowEventHooks>(&Access::changed);
    refresh();
}

WindowTracker::~WindowTracker()
{
    m_hooks.reset();
    g_tracker = nullptr;
}

std::size_t WindowTracker::subscribe(std::function<void()> changed)
{
    const std::size_t id = m_nextSubscriber++;
    m_subscribers.emplace(id, std::move(changed));
    return id;
}

void WindowTracker::unsubscribe(std::size_t id)
{
    m_subscribers.erase(id);
}

void WindowTracker::schedule()
{
    if (m_scheduled)
    {
        return;
    }
    m_scheduled = true;
    slint::Timer::single_shot(kSettle, [this] {
        m_scheduled = false;
        refresh();
    });
}

slint::Image WindowTracker::iconFor(void* window)
{
    const std::wstring identity = providers::appIdentity(window);
    if (!identity.empty())
    {
        if (const auto cached = m_appIcons.find(identity); cached != m_appIcons.end())
        {
            return cached->second;
        }
        const auto pixels = providers::appIconPixels(identity, kAppIconSize);
        if (!pixels.empty())
        {
            if (m_appIcons.size() >= kAppIconsKept)
            {
                m_appIcons.clear();
            }
            return m_appIcons.emplace(identity, imageFromPixels(pixels)).first->second;
        }
    }
    return imageOf(iconHandle(static_cast<HWND>(window)));
}

void WindowTracker::refresh()
{
    const std::vector<void*> current = providers::taskbarWindows();

    bool changed = false;
    std::vector<Entry> next;
    next.reserve(current.size());
    for (auto& entry : m_windows)
    {
        if (std::ranges::find(current, entry.window) == current.end())
        {
            changed = true; // closed, hidden, or moved to another desktop
            continue;
        }
        if (std::string title = providers::windowTitle(entry.window); title != entry.title)
        {
            entry.title = std::move(title);
            entry.iconLoaded = false; // browsers change both together
            changed = true;
        }
        next.push_back(std::move(entry));
    }
    // New windows after the known ones, the oldest (lowest in the z-order) first.
    for (auto it = current.rbegin(); it != current.rend(); ++it)
    {
        if (std::ranges::none_of(next, [&](const Entry& entry) { return entry.window == *it; }))
        {
            next.push_back({*it, providers::windowTitle(*it), {}, false});
            changed = true;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    bool iconsLeft = false;
    for (auto& entry : next)
    {
        if (entry.iconLoaded)
        {
            continue;
        }
        if (std::chrono::steady_clock::now() - started > kIconBudget)
        {
            iconsLeft = true;
            continue;
        }
        entry.icon = iconFor(entry.window);
        entry.iconLoaded = true;
        changed = true;
    }
    if (iconsLeft)
    {
        schedule();
    }
    void* foreground = GetForegroundWindow();
    changed = changed || foreground != m_foreground;
    m_windows = std::move(next);
    m_foreground = foreground;
    if (!changed)
    {
        return;
    }
    const auto subscribers = m_subscribers; // a callback may unsubscribe
    for (const auto& [id, callback] : subscribers)
    {
        callback();
    }
}

void WindowTracker::activate(void* handle)
{
    providers::activateWindow(handle);
}

std::unique_ptr<Provider> makeWindowListProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<WindowListProvider>(slot, instance);
}

} // namespace rmw::barapp
