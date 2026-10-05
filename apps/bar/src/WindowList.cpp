#include "WindowList.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <dwmapi.h>

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

constexpr int kIconSize = 32; // drawn at 20 logical px: sharp up to 160% DPI
constexpr int kMaxTitle = 256;
constexpr auto kSettle = 60ms; // events come in bursts: look once they stop
constexpr UINT kIconTimeoutMs = 50;
// Asking windows for their icons costs up to 3 x 50 ms each: at most this much per pass (the UI
// thread's heartbeat is watched), the rest on the next pass.
constexpr auto kIconBudget = 250ms;

std::atomic<WindowTracker*> g_tracker{nullptr}; // WinEvent callbacks carry no context

bool isTaskbarWindow(HWND window)
{
    if (IsWindowVisible(window) == FALSE || GetAncestor(window, GA_ROOT) != window)
    {
        return false;
    }
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if ((extended & WS_EX_APPWINDOW) == 0 &&
        (GetWindow(window, GW_OWNER) != nullptr || (extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0))
    {
        return false;
    }
    // Cloaked: on another virtual desktop, or a suspended store app.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
    {
        return false;
    }
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process != GetCurrentProcessId() && GetWindowTextLengthW(window) > 0;
}

std::string titleOf(HWND window)
{
    std::array<wchar_t, kMaxTitle> text{};
    const int length = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    return utf16To8Lossy(std::wstring_view(text.data(), static_cast<std::size_t>(std::max(0, length))));
}

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

// Draws an icon into 32-bit pixels and hands them to Slint (straight, not premultiplied, alpha).
slint::Image imageOf(HICON icon)
{
    if (icon == nullptr)
    {
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kIconSize;
    info.bmiHeader.biHeight = -kIconSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* colorBits = nullptr;
    void* maskBits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &colorBits, nullptr, 0);
    HBITMAP mask = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (dc == nullptr || color == nullptr || mask == nullptr)
    {
        for (HBITMAP bitmap : {color, mask})
        {
            if (bitmap != nullptr)
            {
                DeleteObject(bitmap);
            }
        }
        if (dc != nullptr)
        {
            DeleteDC(dc);
        }
        return {};
    }
    HGDIOBJ old = SelectObject(dc, color);
    DrawIconEx(dc, 0, 0, icon, kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
    SelectObject(dc, mask);
    DrawIconEx(dc, 0, 0, icon, kIconSize, kIconSize, 0, nullptr, DI_MASK); // white = transparent
    SelectObject(dc, old);

    constexpr std::size_t kPixels = std::size_t{kIconSize} * kIconSize;
    const std::span pixels(static_cast<const std::uint32_t*>(colorBits), kPixels);
    const std::span maskPixels(static_cast<const std::uint32_t*>(maskBits), kPixels);
    // Old icons have no alpha channel (it stays 0): their mask says what is drawn.
    const bool alpha = std::ranges::any_of(pixels, [](std::uint32_t p) { return (p >> 24) != 0; });
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kIconSize, kIconSize);
    auto* out = buffer.begin();
    for (std::size_t i = 0; i < kPixels; ++i)
    {
        const std::uint32_t p = pixels[i];
        auto a = static_cast<std::uint32_t>(p >> 24);
        if (!alpha)
        {
            a = (maskPixels[i] & 0x00FFFFFFU) != 0 ? 0U : 255U;
        }
        auto r = (p >> 16) & 0xFFU;
        auto g = (p >> 8) & 0xFFU;
        auto b = p & 0xFFU;
        if (alpha && a != 0 && a != 255) // GDI wrote premultiplied color
        {
            r = std::min(255U, r * 255U / a);
            g = std::min(255U, g * 255U / a);
            b = std::min(255U, b * 255U / a);
        }
        out[i] = slint::Rgba8Pixel{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                   static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
    }
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    return {buffer};
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
    static void CALLBACK onEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD,
                                 DWORD)
    {
        if (object != OBJID_WINDOW || child != CHILDID_SELF || window == nullptr)
        {
            return;
        }
        // A destroyed window has no ancestor anymore; anything else must be top-level.
        if (event != EVENT_OBJECT_DESTROY && GetAncestor(window, GA_ROOT) != window)
        {
            return;
        }
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
    const std::array<std::pair<DWORD, DWORD>, 5> ranges = {{
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    }};
    for (const auto& [first, last] : ranges)
    {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, &Access::onEvent, 0, 0,
                                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
        {
            m_hooks.push_back(hook);
        }
    }
    refresh();
}

WindowTracker::~WindowTracker()
{
    for (void* hook : m_hooks)
    {
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook));
    }
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

void WindowTracker::refresh()
{
    std::vector<HWND> current;
    EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            if (isTaskbarWindow(window))
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands our pointer back
                reinterpret_cast<std::vector<HWND>*>(list)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&current));

    bool changed = false;
    std::vector<Entry> next;
    next.reserve(current.size());
    for (auto& entry : m_windows)
    {
        const auto window = static_cast<HWND>(entry.window);
        if (std::ranges::find(current, window) == current.end())
        {
            changed = true; // closed, hidden, or moved to another desktop
            continue;
        }
        if (std::string title = titleOf(window); title != entry.title)
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
            next.push_back({*it, titleOf(*it), {}, false});
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
        entry.icon = imageOf(iconHandle(static_cast<HWND>(entry.window)));
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
    const auto window = static_cast<HWND>(handle);
    if (IsWindow(window) == FALSE)
    {
        return;
    }
    if (window == GetForegroundWindow() && IsIconic(window) == FALSE)
    {
        ShowWindowAsync(window, SW_MINIMIZE);
        return;
    }
    if (IsIconic(window) != FALSE)
    {
        ShowWindowAsync(window, SW_RESTORE);
    }
    // The click was ours, so Windows lets us hand the foreground over; if it still refuses (the
    // foreground lock), do what Alt+Tab does.
    if (SetForegroundWindow(window) == FALSE)
    {
        SwitchToThisWindow(window, TRUE);
    }
}

std::unique_ptr<Provider> makeWindowListProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<WindowListProvider>(slot, instance);
}

} // namespace rmw::barapp
