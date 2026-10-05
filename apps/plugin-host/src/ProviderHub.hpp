#pragma once

#include "rmw/domain/Value.hpp"
#include "rmw/providers/Media.hpp"
#include "rmw/providers/System.hpp"
#include "rmw/providers/Volume.hpp"
#include "rmw/providers/Windows.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::pluginhost
{

using Clock = std::chrono::steady_clock;

// The providers of docs/12 §5 as plugins see them (rmw.provider / rmw.on), sampled only while some
// plugin listens. Cheap ones are polled from the host's loop; volume, media and the window list
// tell us when they change (their callbacks come from other threads through `post`).
class ProviderHub
{
public:
    using Post = std::function<void(std::function<void()>)>;
    using Changed = std::function<void(std::string_view name, const domain::Value& value)>;

    ProviderHub(Post post, Changed changed);
    ~ProviderHub();
    ProviderHub(const ProviderHub&) = delete;
    ProviderHub& operator=(const ProviderHub&) = delete;
    ProviderHub(ProviderHub&&) = delete;
    ProviderHub& operator=(ProviderHub&&) = delete;

    // The latest value (sampled now when nothing is known yet).
    [[nodiscard]] domain::Value value(std::string_view name);
    void watch(std::span<const std::string> names);
    // Polls what is due. Returns when it wants to be called next.
    [[nodiscard]] Clock::time_point poll(Clock::time_point now);

    // media.control
    void media(std::string_view command);

    // Main thread: a window came, went, changed its title or took the foreground.
    void windowsChanged();

    class WindowThread;

private:
    struct Polled
    {
        std::chrono::milliseconds every{0};
        Clock::time_point due;
    };

    void publish(std::string_view name, domain::Value value);
    [[nodiscard]] std::optional<domain::Value> sample(std::string_view name);
    void startWatchers(std::string_view name);
    void stopWatchers();
    [[nodiscard]] domain::Value windowList() const;

    Post m_post;
    Changed m_changed;
    std::vector<std::string> m_watched;
    std::map<std::string, domain::Value, std::less<>> m_values;
    std::map<std::string, Polled, std::less<>> m_polled;
    providers::CpuSampler m_cpu;
    providers::NetworkSampler m_network;
    std::unique_ptr<providers::VolumeWatcher> m_volume;
    std::unique_ptr<providers::MediaWatcher> m_media;
    std::unique_ptr<WindowThread> m_windowThread;
    std::optional<Clock::time_point> m_windowsDue;
};

} // namespace rmw::pluginhost
