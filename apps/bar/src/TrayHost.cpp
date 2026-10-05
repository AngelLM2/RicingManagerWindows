#include "TrayHost.hpp"

#include "TrayForwarder.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/NativeTaskbar.hpp"

#include <windows.h>

#include <shlobj.h>
#include <wil/resource.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>

namespace rmw::barapp
{
namespace
{

using namespace std::chrono_literals;
namespace tray = bar::tray;

constexpr const wchar_t* kTrayClass = L"Shell_TrayWnd";
constexpr const wchar_t* kSettingsKey = L"Control Panel\\NotifyIconSettings";
constexpr UINT kStopMessage = WM_APP + 1;
constexpr UINT_PTR kFrontTimer = 1;
constexpr UINT_PTR kPublishTimer = 2;
constexpr UINT_PTR kInviteTimer = 3;
constexpr UINT kFrontIntervalMs = 250;   // apps that find Explorer's window first are missed
constexpr UINT kPublishDelayMs = 50;     // bursts of updates reach the UI once
constexpr UINT kInviteAgainMs = 2000;    // after Explorer restarts, for apps that raced us
constexpr int kPruneEveryTicks = 20;     // dead owners every 5 s
constexpr UINT kForwardTimeoutMs = 1500; // app bars (ours too) wait for this: below the bar's hang time
constexpr std::size_t kMaxSettings = 2048;
constexpr auto kStopDelay = 3s;
constexpr auto kAnchorLife = 5s;
constexpr int kAnchorHalf = 16; // around the click: where the app may anchor its menu or flyout

// Window and icon handles travel as 32 bits; a 64-bit process sign-extends them.
HWND windowOf(std::uint32_t handle) noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<HWND>(static_cast<std::intptr_t>(static_cast<std::int32_t>(handle)));
}

HICON iconOf(std::uint32_t handle) noexcept
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<HICON>(static_cast<std::intptr_t>(static_cast<std::int32_t>(handle)));
}

bool ownerGone(std::uint32_t owner)
{
    return IsWindow(windowOf(owner)) == FALSE;
}

std::wstring processPath(DWORD pid)
{
    const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
    {
        return {};
    }
    std::array<wchar_t, 1024> buffer{};
    auto size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size) == FALSE)
    {
        return {};
    }
    return {buffer.data(), size};
}

// "{6D809377-...}\App\app.exe" -> "C:\Program Files\App\app.exe" (Windows writes known folders so).
std::wstring expandKnownFolder(std::wstring path)
{
    if (!path.starts_with(L'{'))
    {
        return path;
    }
    const auto close = path.find(L'}');
    if (close == std::wstring::npos)
    {
        return path;
    }
    GUID folder{};
    if (FAILED(CLSIDFromString(path.substr(0, close + 1).c_str(), &folder)))
    {
        return path;
    }
    wil::unique_cotaskmem_string root;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &root)))
    {
        return path;
    }
    return std::wstring(root.get()) + path.substr(close + 1);
}

bool samePathIgnoringCase(std::wstring_view a, std::wstring_view b)
{
    return !a.empty() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Windows 11 keeps, per app icon, whether it shows on the taskbar or only in the overflow
// ("Other system tray icons" in Settings).
struct IconSetting
{
    std::wstring path;
    std::optional<std::uint32_t> uid;
    bool promoted = false;
};

std::optional<std::uint32_t> readDword(HKEY key, const wchar_t* name)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return value;
}

std::wstring readString(HKEY key, const wchar_t* name)
{
    std::array<wchar_t, 1024> buffer{};
    auto size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
    {
        return {};
    }
    return buffer.data();
}

// The tray thread: the window, the model, the copies of the icons.
class Worker
{
public:
    explicit Worker(std::shared_ptr<TrayHost::Shared> shared);
    void run(std::promise<void>& ready);

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT onCopyData(WPARAM wParam, LPARAM lParam);
    LRESULT forward(UINT message, WPARAM wParam, LPARAM lParam, bool& delivered) const;
    void apply(const tray::Update& update);
    std::optional<LRESULT> answerRect(const tray::RectQuery& query);
    void keepInFront();
    void readSettings();
    void watchSettings();
    [[nodiscard]] bool promotedFor(const TrayHost::IconView& view) const;
    void schedulePublish();
    void publish();

    std::shared_ptr<TrayHost::Shared> m_shared;
    std::unique_ptr<TrayForwarder> m_forwarder; // Explorer's copy, kept in step
    HWND m_window = nullptr;
    UINT m_taskbarCreated = 0;
    tray::Model m_model;
    std::map<std::uint64_t, TrayHost::IconView> m_views;
    std::map<DWORD, std::wstring> m_paths; // by process
    wil::unique_hkey m_settingsKey;
    wil::unique_event m_settingsChanged;
    std::vector<IconSetting> m_settings;
    bool m_haveSettings = false; // Windows 10 keeps them elsewhere: everything shows
    bool m_publishScheduled = false;
    std::uint64_t m_revision = 0;
    int m_ticks = 0;
};

} // namespace

struct TrayHost::Shared
{
    struct Anchor
    {
        std::uint32_t owner = 0;
        std::uint32_t id = 0;
        std::optional<tray::Guid> guid;
        RECT rect{};
        std::chrono::steady_clock::time_point at;
    };
    std::mutex mutex;
    std::optional<Anchor> anchor; // the last icon the user clicked, and where
    std::atomic<HWND> window{nullptr};
};

namespace
{

Worker::Worker(std::shared_ptr<TrayHost::Shared> shared)
    : m_shared(std::move(shared))
    , m_taskbarCreated(RegisterWindowMessageW(L"TaskbarCreated"))
{
}

void Worker::run(std::promise<void>& ready)
{
    MSG peek{};
    PeekMessageW(&peek, nullptr, 0, 0, PM_NOREMOVE); // this thread's queue, before anyone posts to it
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Worker::proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kTrayClass;
    RegisterClassExW(&windowClass); // a class of this process: Explorer's is in its own
    m_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kTrayClass, L"", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_window == nullptr)
    {
        log::error("tray: cannot create the tray window ({})", GetLastError());
        ready.set_value();
        return;
    }
    SetPropW(m_window, platform::kTraySpyProperty,
             reinterpret_cast<HANDLE>(1)); // NOLINT(performance-no-int-to-ptr)
    SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Apps of a lower integrity level (sandboxed ones) send to it too.
    ChangeWindowMessageFilterEx(m_window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_window, m_taskbarCreated, MSGFLT_ALLOW, nullptr);
    m_shared->window = m_window;
    SetTimer(m_window, kFrontTimer, kFrontIntervalMs, nullptr);
    readSettings();
    watchSettings();
    m_forwarder = std::make_unique<TrayForwarder>();
    ready.set_value();
    log::info("tray: the tray window is up; asking apps for their icons");
    platform::askAppsForTrayIcons();

    bool running = true;
    while (running)
    {
        HANDLE events[] = {m_settingsChanged.get()}; // NOLINT(modernize-avoid-c-arrays)
        const DWORD count = m_settingsChanged ? 1 : 0;
        const DWORD signaled =
            MsgWaitForMultipleObjectsEx(count, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (count == 1 && signaled == WAIT_OBJECT_0)
        {
            readSettings();
            watchSettings();
            for (auto& [number, view] : m_views)
            {
                view.promoted = promotedFor(view);
            }
            schedulePublish();
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            DispatchMessageW(&message);
        }
    }
    m_shared->window = nullptr;
    m_forwarder.reset();
    KillTimer(m_window, kFrontTimer);
    RemovePropW(m_window, platform::kTraySpyProperty);
    SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
    DestroyWindow(m_window);
    UnregisterClassW(kTrayClass, GetModuleHandleW(nullptr));
    // Explorer's tray may lack what it refused: apps give it their icons themselves now.
    platform::askAppsForTrayIcons();
    log::info("tray: the tray window is gone; apps keep their icons in Explorer's tray");
}

LRESULT CALLBACK Worker::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* self = reinterpret_cast<Worker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr)
    {
        return self->handle(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Worker::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_COPYDATA)
    {
        return onCopyData(wParam, lParam);
    }
    if (message == WM_TIMER)
    {
        if (wParam == kFrontTimer)
        {
            keepInFront();
            if (++m_ticks % kPruneEveryTicks == 0)
            {
                for (const std::uint64_t number : m_model.removeIf(&ownerGone))
                {
                    m_views.erase(number);
                    log::info("tray: icon {} went away with its app", number);
                    schedulePublish();
                }
            }
        }
        else if (wParam == kPublishTimer)
        {
            KillTimer(m_window, kPublishTimer);
            m_publishScheduled = false;
            publish();
        }
        else if (wParam == kInviteTimer)
        {
            KillTimer(m_window, kInviteTimer);
            platform::askAppsForTrayIcons();
        }
        return 0;
    }
    if (message == kStopMessage)
    {
        PostQuitMessage(0);
        return 0;
    }
    if (message == m_taskbarCreated && m_taskbarCreated != 0)
    {
        // Explorer (re)started its tray: apps add their icons again now. Be in front for it, and
        // ask once more in a moment for those that reached Explorer's window first.
        keepInFront();
        SetTimer(m_window, kInviteTimer, kInviteAgainMs, nullptr);
        return 0;
    }
    // What other programs ask of "the taskbar" (WM_USER messages, commands like "minimize all")
    // is Explorer's to answer.
    if ((message >= WM_USER && message < WM_APP) || message == WM_COMMAND || message == WM_SYSCOMMAND)
    {
        if (auto* const real = static_cast<HWND>(platform::nativeTrayWindow()))
        {
            if (InSendMessage() != FALSE)
            {
                bool delivered = false;
                return forward(message, wParam, lParam, delivered);
            }
            PostMessageW(real, message, wParam, lParam);
            return 0;
        }
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

LRESULT Worker::forward(UINT message, WPARAM wParam, LPARAM lParam, bool& delivered) const
{
    delivered = false;
    auto* const real = static_cast<HWND>(platform::nativeTrayWindow());
    if (real == nullptr)
    {
        return 0;
    }
    DWORD_PTR result = 0;
    delivered =
        SendMessageTimeoutW(real, message, wParam, lParam, SMTO_ABORTIFHUNG, kForwardTimeoutMs, &result) != 0;
    return static_cast<LRESULT>(result);
}

LRESULT Worker::onCopyData(WPARAM wParam, LPARAM lParam)
{
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam); // NOLINT(performance-no-int-to-ptr)
    if (data != nullptr && data->lpData != nullptr && data->dwData == tray::kNotifyIconData)
    {
        const std::span bytes(static_cast<const std::byte*>(data->lpData), data->cbData);
        if (const auto update = tray::decodeUpdate(bytes))
        {
            // The app hears "done" now; Explorer gets it from the forwarder (Explorer may be slow,
            // and app bars — ours too — must not wait behind every app's icons).
            apply(*update);
            m_forwarder->send({bytes.begin(), bytes.end()}, wParam, *update);
            return TRUE;
        }
    }
    if (data != nullptr && data->lpData != nullptr && data->dwData == tray::kIconRectQuery)
    {
        const std::span bytes(static_cast<const std::byte*>(data->lpData), data->cbData);
        if (const auto query = tray::decodeRectQuery(bytes))
        {
            if (const auto answer = answerRect(*query))
            {
                return *answer;
            }
        }
    }
    // App bars and anything else: Explorer answers, now.
    bool delivered = false;
    return forward(WM_COPYDATA, wParam, lParam, delivered);
}

void Worker::apply(const tray::Update& update)
{
    const auto result = m_model.apply(update);
    if (result.change == tray::Model::Change::None)
    {
        return;
    }
    if (result.change == tray::Model::Change::Removed)
    {
        m_views.erase(result.number);
        log::info("tray: icon {} removed", result.number);
        schedulePublish();
        return;
    }
    const tray::Icon* icon = m_model.find(result.number);
    if (icon == nullptr)
    {
        return;
    }
    TrayHost::IconView& view = m_views[result.number];
    view.icon = *icon;
    if (result.change == tray::Model::Change::Added)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(windowOf(icon->owner), &pid);
        view.pid = pid;
        auto& path = m_paths[pid];
        if (path.empty())
        {
            path = processPath(pid);
        }
        view.app = utf16To8Lossy(std::filesystem::path(path).filename().native());
        view.promoted = promotedFor(view);
        log::info("tray: icon {} added by {} (pid {})", result.number, view.app.empty() ? "?" : view.app,
                  pid);
    }
    if (result.iconChanged)
    {
        auto pixels = providers::iconPixels(iconOf(icon->icon));
        // A handle the app destroyed already: keep the last picture we had.
        if (!pixels.empty() || icon->icon == 0)
        {
            view.pixels = std::move(pixels);
            view.revision = ++m_revision;
        }
    }
    view.tip = utf16To8Lossy(std::wstring(icon->tip.begin(), icon->tip.end()));
    schedulePublish();
}

std::optional<LRESULT> Worker::answerRect(const tray::RectQuery& query)
{
    const std::scoped_lock lock(m_shared->mutex);
    const auto& anchor = m_shared->anchor;
    if (!anchor || std::chrono::steady_clock::now() - anchor->at > kAnchorLife)
    {
        return std::nullopt;
    }
    const bool same = query.guid && anchor->guid ? query.guid == anchor->guid
                                                 : query.owner == anchor->owner && query.id == anchor->id;
    if (!same)
    {
        return std::nullopt;
    }
    // Where the user clicked on our bar, not where Explorer's hidden tray would be.
    return query.size ? tray::packPoint(anchor->rect.right - anchor->rect.left,
                                        anchor->rect.bottom - anchor->rect.top)
                      : tray::packPoint(anchor->rect.left, anchor->rect.top);
}

void Worker::keepInFront()
{
    if (FindWindowW(kTrayClass, nullptr) != m_window)
    {
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void Worker::readSettings()
{
    m_settings.clear();
    if (!m_settingsKey)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ | KEY_NOTIFY, &key) != ERROR_SUCCESS)
        {
            m_haveSettings = false;
            return;
        }
        m_settingsKey.reset(key);
    }
    m_haveSettings = true;
    std::array<wchar_t, 256> name{};
    for (DWORD index = 0; index < kMaxSettings; ++index)
    {
        auto length = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExW(m_settingsKey.get(), index, name.data(), &length, nullptr,
                                             nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
        {
            break;
        }
        wil::unique_hkey entry;
        if (RegOpenKeyExW(m_settingsKey.get(), name.data(), 0, KEY_READ, &entry) != ERROR_SUCCESS)
        {
            continue;
        }
        IconSetting setting;
        setting.path = expandKnownFolder(readString(entry.get(), L"ExecutablePath"));
        setting.uid = readDword(entry.get(), L"UID");
        setting.promoted = readDword(entry.get(), L"IsPromoted").value_or(0) != 0;
        if (!setting.path.empty())
        {
            m_settings.push_back(std::move(setting));
        }
    }
}

void Worker::watchSettings()
{
    if (!m_settingsKey)
    {
        return;
    }
    if (!m_settingsChanged)
    {
        m_settingsChanged.create(wil::EventOptions::None);
    }
    if (RegNotifyChangeKeyValue(m_settingsKey.get(), TRUE,
                                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, m_settingsChanged.get(),
                                TRUE) != ERROR_SUCCESS)
    {
        m_settingsChanged.reset();
    }
}

bool Worker::promotedFor(const TrayHost::IconView& view) const
{
    if (!m_haveSettings)
    {
        return true;
    }
    const auto found = m_paths.find(view.pid);
    if (found == m_paths.end() || found->second.empty())
    {
        return true; // we cannot tell: better shown than lost
    }
    for (const auto& setting : m_settings)
    {
        if (samePathIgnoringCase(setting.path, found->second) &&
            (!setting.uid || *setting.uid == view.icon.id))
        {
            return setting.promoted;
        }
    }
    return false; // Windows puts new icons in the overflow until the user promotes them
}

void Worker::schedulePublish()
{
    if (!m_publishScheduled)
    {
        m_publishScheduled = true;
        SetTimer(m_window, kPublishTimer, kPublishDelayMs, nullptr);
    }
}

void Worker::publish()
{
    std::vector<TrayHost::IconView> list;
    list.reserve(m_model.icons().size());
    for (const auto& icon : m_model.icons())
    {
        if (const auto found = m_views.find(icon.number); found != m_views.end())
        {
            list.push_back(found->second);
        }
    }
    slint::invoke_from_event_loop([shared = m_shared, list = std::move(list)]() mutable {
        TrayHost::instance().receive(shared, std::move(list));
    });
}

} // namespace

TrayHost& TrayHost::instance()
{
    static TrayHost host;
    return host;
}

TrayHost::TrayHost() = default;

TrayHost::~TrayHost()
{
    stop();
}

std::size_t TrayHost::subscribe(std::function<void()> changed)
{
    if (m_stopTimer)
    {
        m_stopTimer->stop();
    }
    const std::size_t id = m_nextSubscriber++;
    m_subscribers.emplace(id, std::move(changed));
    if (!m_thread.joinable())
    {
        start();
    }
    return id;
}

void TrayHost::unsubscribe(std::size_t id)
{
    m_subscribers.erase(id);
    if (m_subscribers.empty())
    {
        if (!m_stopTimer)
        {
            m_stopTimer = std::make_unique<slint::Timer>();
        }
        m_stopTimer->start(slint::TimerMode::SingleShot, kStopDelay, [this] {
            if (m_subscribers.empty())
            {
                stop();
            }
        });
    }
}

void TrayHost::shutdown()
{
    m_stopTimer.reset();
    m_subscribers.clear();
    stop();
}

const TrayHost::IconView* TrayHost::find(std::uint64_t number) const noexcept
{
    const auto found =
        std::ranges::find_if(m_icons, [number](const IconView& view) { return view.icon.number == number; });
    return found == m_icons.end() ? nullptr : &*found;
}

void TrayHost::start()
{
    m_shared = std::make_shared<Shared>();
    std::promise<void> ready;
    auto started = ready.get_future();
    m_thread = std::jthread([shared = m_shared, ready = std::move(ready)]() mutable {
        Worker worker(shared);
        worker.run(ready);
    });
    if (started.wait_for(2s) != std::future_status::ready)
    {
        log::warn("tray: the tray window is slow to start");
    }
}

void TrayHost::stop()
{
    if (!m_thread.joinable())
    {
        return;
    }
    if (HWND window = m_shared->window.load())
    {
        PostMessageW(window, kStopMessage, 0, 0);
    }
    m_thread.join();
    m_thread = {};
    m_shared.reset();
    m_icons.clear();
}

void TrayHost::receive(const std::shared_ptr<Shared>& from, std::vector<IconView> icons)
{
    if (from != m_shared)
    {
        return; // from a tray window that is gone
    }
    m_icons = std::move(icons);
    // Copied: a subscriber may unsubscribe while being told.
    const auto subscribers = m_subscribers;
    for (const auto& [id, changed] : subscribers)
    {
        changed();
    }
}

void TrayHost::gesture(std::uint64_t number, tray::Gesture gesture, int x, int y)
{
    const IconView* view = find(number);
    if (view == nullptr || !m_shared)
    {
        return;
    }
    const tray::Icon& icon = view->icon;
    const bool click = gesture != tray::Gesture::HoverStart && gesture != tray::Gesture::HoverEnd;
    if (click)
    {
        {
            const std::scoped_lock lock(m_shared->mutex);
            m_shared->anchor =
                Shared::Anchor{icon.owner, icon.id, icon.guid,
                               RECT{x - kAnchorHalf, y - kAnchorHalf, x + kAnchorHalf, y + kAnchorHalf},
                               std::chrono::steady_clock::now()};
        }
        // The app shows its menu or window in answer: let it come to the front.
        AllowSetForegroundWindow(view->pid);
        log::info("tray: {} on icon {} ({})",
                  gesture == tray::Gesture::LeftClick     ? "left click"
                  : gesture == tray::Gesture::RightClick  ? "right click"
                  : gesture == tray::Gesture::DoubleClick ? "double click"
                                                          : "middle click",
                  number, view->app);
    }
    for (const auto& post : tray::postsFor(icon, gesture, x, y))
    {
        PostMessageW(windowOf(icon.owner), icon.callback, post.wParam, post.lParam);
    }
}

} // namespace rmw::barapp
