#include "ProviderHub.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <ctime>
#include <future>
#include <thread>

namespace rmw::pluginhost
{
namespace
{

using namespace std::chrono_literals;
using domain::Value;

constexpr auto kWindowsSettle = 100ms; // window events come in bursts: look once they stop

std::atomic<ProviderHub*> g_hub{nullptr}; // the WinEvent hooks carry no context

struct Poll
{
    std::string_view name;
    std::chrono::milliseconds every;
};

constexpr auto kPolled = std::to_array<Poll>({
    {"time", 1000ms},
    {"system.cpu", 1000ms},
    {"system.memory", 2000ms},
    {"network.stats", 1000ms},
    {"battery", 10'000ms},
    {"monitors", 5000ms},
    {"virtual-desktops", 1000ms},
    // Also event-driven; polled too because window events were seen to arrive seconds late.
    {"windows.list", 1000ms},
});

Value number(double value)
{
    return Value{std::round(value * 10.0) / 10.0};
}

Value integer(std::int64_t value)
{
    return Value{value};
}

Value table(Value::Table members)
{
    std::ranges::sort(members, {}, &domain::ValueMember::key);
    return Value{std::move(members)};
}

std::int64_t utcOffsetMinutes()
{
    _tzset(); // the time zone as it is now (the C runtime reads it once)
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    std::tm utc{};
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
    utc.tm_isdst = local.tm_isdst;
    return static_cast<std::int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60.0);
}

} // namespace

// The window events (WinEvent hooks, out of context) on a thread of their own, in a GetMessage loop:
// the host's main loop waits with MsgWaitForMultipleObjectsEx, which does not wake for them, so
// they reached it seconds late. Each event goes to the main thread through the inbox.
class ProviderHub::WindowThread
{
public:
    explicit WindowThread(Post post)
    {
        std::promise<DWORD> started;
        auto id = started.get_future();
        m_thread = std::jthread([post = std::move(post), &started] {
            static thread_local const Post* forward = nullptr;
            forward = &post;
            const providers::WindowEventHooks hooks([] {
                (*forward)([] {
                    if (auto* hub = g_hub.load())
                    {
                        hub->windowsChanged();
                    }
                });
            });
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // the queue exists from here on
            started.set_value(GetCurrentThreadId());
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        });
        m_id = id.get();
    }

    ~WindowThread() { PostThreadMessageW(m_id, WM_QUIT, 0, 0); }
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;
    WindowThread(WindowThread&&) = delete;
    WindowThread& operator=(WindowThread&&) = delete;

private:
    DWORD m_id = 0;
    std::jthread m_thread; // joined after WM_QUIT
};

ProviderHub::ProviderHub(Post post, Changed changed)
    : m_post(std::move(post))
    , m_changed(std::move(changed))
{
    g_hub = this;
}

ProviderHub::~ProviderHub()
{
    stopWatchers();
    m_media.reset();
    g_hub = nullptr;
}

Value ProviderHub::value(std::string_view name)
{
    if (const auto found = m_values.find(name); found != m_values.end())
    {
        return found->second;
    }
    startWatchers(name);
    if (auto sampled = sample(name))
    {
        m_values[std::string(name)] = *sampled;
        return *sampled;
    }
    return {};
}

void ProviderHub::watch(std::span<const std::string> names)
{
    m_watched.assign(names.begin(), names.end());
    const auto now = Clock::now();
    std::erase_if(m_polled, [&](const auto& entry) {
        return std::ranges::find(m_watched, entry.first) == m_watched.end();
    });
    for (const auto& name : m_watched)
    {
        for (const auto& poll : kPolled)
        {
            if (poll.name == name && !m_polled.contains(name))
            {
                m_polled.emplace(name, Polled{poll.every, now});
            }
        }
        startWatchers(name);
    }
    stopWatchers();
}

Clock::time_point ProviderHub::poll(Clock::time_point now)
{
    Clock::time_point next = now + 1h;
    for (auto& [name, entry] : m_polled)
    {
        if (entry.due <= now)
        {
            if (auto sampled = sample(name))
            {
                publish(name, std::move(*sampled));
            }
            entry.due = now + entry.every;
        }
        next = std::min(next, entry.due);
    }
    if (m_windowsDue)
    {
        if (*m_windowsDue <= now)
        {
            m_windowsDue.reset();
            publish("windows.list", windowList());
        }
        else
        {
            next = std::min(next, *m_windowsDue);
        }
    }
    return next;
}

void ProviderHub::media(std::string_view command)
{
    if (!m_media)
    {
        startWatchers("media");
    }
    if (command == "toggle")
    {
        m_media->toggle();
    }
    else if (command == "play")
    {
        m_media->play();
    }
    else if (command == "pause")
    {
        m_media->pause();
    }
    else if (command == "next")
    {
        m_media->next();
    }
    else if (command == "previous")
    {
        m_media->previous();
    }
}

void ProviderHub::fromBar(std::string_view name, domain::Value value)
{
    publish(name, std::move(value));
}

void ProviderHub::windowsChanged()
{
    if (!m_windowsDue)
    {
        m_windowsDue = Clock::now() + kWindowsSettle;
    }
}

void ProviderHub::publish(std::string_view name, Value value)
{
    auto& slot = m_values[std::string(name)];
    if (slot == value)
    {
        return; // nothing new: no callback
    }
    slot = std::move(value);
    if (std::ranges::find(m_watched, name) != m_watched.end())
    {
        m_changed(name, slot);
    }
}

std::optional<Value> ProviderHub::sample(std::string_view name)
{
    if (name == "time")
    {
        return table({{"unix", integer(std::time(nullptr))}, {"offset", integer(utcOffsetMinutes())}});
    }
    if (name == "system.cpu")
    {
        if (const auto load = m_cpu.sample())
        {
            return table({{"usage", number(*load)}});
        }
        return std::nullopt; // the first sample only primes the counters
    }
    if (name == "system.memory")
    {
        if (const auto memory = providers::sampleMemory())
        {
            return table({{"usage", integer(memory->load)},
                          {"total", integer(static_cast<std::int64_t>(memory->totalBytes))},
                          {"available", integer(static_cast<std::int64_t>(memory->availableBytes))}});
        }
        return std::nullopt;
    }
    if (name == "network.stats")
    {
        if (const auto rates = m_network.sample())
        {
            return table(
                {{"down", number(rates->downBytesPerSecond)}, {"up", number(rates->upBytesPerSecond)}});
        }
        return std::nullopt;
    }
    if (name == "battery")
    {
        const auto battery = providers::sampleBattery();
        return table({{"present", Value{battery.present}},
                      {"percent", integer(battery.percent)},
                      {"plugged_in", Value{battery.pluggedIn}},
                      {"charging", Value{battery.charging}}});
    }
    if (name == "monitors")
    {
        Value::Array list;
        for (const auto& monitor : providers::listMonitors())
        {
            list.push_back(table({{"name", Value{monitor.name}},
                                  {"x", integer(monitor.x)},
                                  {"y", integer(monitor.y)},
                                  {"width", integer(monitor.width)},
                                  {"height", integer(monitor.height)},
                                  {"dpi", integer(monitor.dpi)},
                                  {"primary", Value{monitor.primary}}}));
        }
        return Value{std::move(list)};
    }
    if (name == "virtual-desktops")
    {
        const auto desktops = providers::readVirtualDesktops();
        return table({{"count", integer(desktops.count)}, {"current", integer(desktops.current + 1)}});
    }
    if (name == "windows.list")
    {
        return windowList();
    }
    return std::nullopt; // event-driven: the watcher will tell
}

void ProviderHub::startWatchers(std::string_view name)
{
    if (name == "audio.volume" && !m_volume)
    {
        m_volume =
            std::make_unique<providers::VolumeWatcher>([post = m_post](const providers::VolumeState& state) {
                post([state] {
                    if (auto* hub = g_hub.load())
                    {
                        hub->publish("audio.volume",
                                     table({{"device", Value{state.device}},
                                            {"level", integer(std::lround(state.level * 100.0F))},
                                            {"muted", Value{state.muted}}}));
                    }
                });
            });
    }
    else if (name == "media" && !m_media)
    {
        m_media =
            std::make_unique<providers::MediaWatcher>([post = m_post](const providers::MediaState& state) {
                // Built here (a pool thread): only the finished value goes to the main thread.
                Value media = table({{"session", Value{state.session}},
                                     {"playing", Value{state.playing}},
                                     {"title", Value{state.title}},
                                     {"artist", Value{state.artist}},
                                     {"album", Value{state.album}},
                                     {"app", Value{state.app}}});
                post([media = std::move(media)] {
                    if (auto* hub = g_hub.load())
                    {
                        hub->publish("media", media);
                    }
                });
            });
    }
    else if (name == "windows.list" && !m_windowThread)
    {
        m_windowThread = std::make_unique<WindowThread>(m_post);
    }
}

void ProviderHub::stopWatchers()
{
    const auto watched = [this](std::string_view name) {
        return std::ranges::find(m_watched, name) != m_watched.end();
    };
    if (!watched("audio.volume"))
    {
        m_volume.reset();
    }
    if (!watched("windows.list"))
    {
        m_windowThread.reset();
    }
    // The media watcher stays once started: media.control needs it too.
}

Value ProviderHub::windowList() const
{
    Value::Array list;
    void* foreground = GetForegroundWindow();
    for (void* window : providers::taskbarWindows())
    {
        list.push_back(
            table({{"id", integer(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(window)))},
                   {"title", Value{providers::windowTitle(window)}},
                   {"app", Value{providers::windowApp(window)}},
                   {"active", Value{window == foreground}},
                   {"minimized", Value{IsIconic(static_cast<HWND>(window)) != FALSE}}}));
    }
    return Value{std::move(list)};
}

} // namespace rmw::pluginhost
