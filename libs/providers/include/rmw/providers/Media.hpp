#pragma once

#include <functional>
#include <memory>
#include <string>

namespace rmw::providers
{

struct MediaState
{
    bool session = false; // something can be controlled
    bool playing = false;
    std::string title;
    std::string artist;
    std::string album;
    std::string app; // the player's app id ("Spotify.exe", a Store app's AppUserModelID)

    friend bool operator==(const MediaState&, const MediaState&) = default;
};

// What Windows is playing (what shows in the volume flyout's media controls), through the system
// media transport controls. Every WinRT call goes to another process and may take its time, so
// none runs on the caller's thread: they run on pool threads.
class MediaWatcher
{
public:
    // `changed` runs on a pool thread. It may run once more while the watcher is being destroyed:
    // capture only what outlives it (a shared handle).
    explicit MediaWatcher(std::function<void(const MediaState&)> changed);
    ~MediaWatcher();
    MediaWatcher(const MediaWatcher&) = delete;
    MediaWatcher& operator=(const MediaWatcher&) = delete;
    MediaWatcher(MediaWatcher&&) = delete;
    MediaWatcher& operator=(MediaWatcher&&) = delete;

    void toggle();
    void play();
    void pause();
    void next();
    void previous();

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

} // namespace rmw::providers
