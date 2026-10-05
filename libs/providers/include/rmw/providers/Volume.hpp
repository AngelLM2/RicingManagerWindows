#pragma once

#include <functional>
#include <memory>

namespace rmw::providers
{

struct VolumeState
{
    bool device = false; // false: no output device
    float level = 0.0F;  // 0..1
    bool muted = false;
};

// The default output device's volume. Core Audio calls go to a Windows service and may take
// seconds (it starts late, a device changes): they all run on a thread of their own, never on the
// caller's (a UI thread whose heartbeat is watched). Reconnects when the default device changes.
class VolumeWatcher
{
public:
    // `changed` runs on the audio thread: hand the state over to your own thread. It may run once
    // more while the watcher is being destroyed: capture only what outlives it (a shared handle).
    explicit VolumeWatcher(std::function<void(const VolumeState&)> changed);
    ~VolumeWatcher();
    VolumeWatcher(const VolumeWatcher&) = delete;
    VolumeWatcher& operator=(const VolumeWatcher&) = delete;
    VolumeWatcher(VolumeWatcher&&) = delete;
    VolumeWatcher& operator=(VolumeWatcher&&) = delete;

    void toggleMute();
    void step(float delta); // -1..1, added to the level

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace rmw::providers
