#include "rmw/providers/Media.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <objbase.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <mutex>

namespace rmw::providers
{

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

struct MediaWatcher::Shared
{
    std::mutex mutex;
    bool alive = true;
    std::function<void(const MediaState&)> listener;
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    GlobalSystemMediaTransportControlsSession session{nullptr};
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker sessionChanged;
    GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker propertiesChanged;
    GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker playbackChanged;

    GlobalSystemMediaTransportControlsSession current()
    {
        const std::scoped_lock lock(mutex);
        return session;
    }

    void detach()
    {
        const std::scoped_lock lock(mutex);
        sessionChanged.revoke();
        propertiesChanged.revoke();
        playbackChanged.revoke();
        session = nullptr;
        manager = nullptr;
    }
};

namespace
{

using Shared = MediaWatcher::Shared;

// Runs `work` on a thread-pool thread, in the multithreaded apartment.
void background(std::function<void()> work)
{
    auto task = std::make_unique<std::function<void()>>(std::move(work));
    const BOOL submitted = TrySubmitThreadpoolCallback(
        [](PTP_CALLBACK_INSTANCE, void* context) {
            const std::unique_ptr<std::function<void()>> run(static_cast<std::function<void()>*>(context));
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            try
            {
                (*run)();
            }
            catch (const winrt::hresult_error& e)
            {
                log::warn("media: {:#x}", static_cast<unsigned>(e.code().value));
            }
            catch (const std::exception& e)
            {
                log::warn("media: {}", e.what());
            }
            if (com)
            {
                CoUninitialize();
            }
        },
        task.get(), nullptr);
    if (submitted != FALSE)
    {
        [[maybe_unused]] auto* handedOver = task.release();
    }
}

void publish(const std::shared_ptr<Shared>& shared, const MediaState& state)
{
    std::function<void(const MediaState&)> listener;
    {
        const std::scoped_lock lock(shared->mutex);
        if (!shared->alive)
        {
            return;
        }
        listener = shared->listener;
    }
    if (listener)
    {
        listener(state);
    }
}

// Pool thread.
void refresh(const std::shared_ptr<Shared>& shared)
{
    const auto session = shared->current();
    if (!session)
    {
        publish(shared, {});
        return;
    }
    MediaState state;
    state.session = true;
    state.playing = session.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    state.app = winrt::to_string(session.SourceAppUserModelId());
    const auto properties = session.TryGetMediaPropertiesAsync().get();
    state.title = winrt::to_string(properties.Title());
    state.artist = winrt::to_string(properties.Artist());
    state.album = winrt::to_string(properties.AlbumTitle());
    publish(shared, state);
}

// Pool thread: a player opened, closed or took over.
void follow(const std::shared_ptr<Shared>& shared)
{
    {
        const std::scoped_lock lock(shared->mutex);
        if (!shared->alive || !shared->manager)
        {
            return;
        }
        shared->propertiesChanged.revoke();
        shared->playbackChanged.revoke();
        auto session = shared->manager.GetCurrentSession();
        shared->session = session;
        if (session)
        {
            const auto changed = [weak = std::weak_ptr(shared)](auto&&, auto&&) {
                if (auto alive = weak.lock())
                {
                    refresh(alive);
                }
            };
            shared->propertiesChanged = session.MediaPropertiesChanged(winrt::auto_revoke, changed);
            shared->playbackChanged = session.PlaybackInfoChanged(winrt::auto_revoke, changed);
        }
    }
    refresh(shared);
}

// Pool thread.
void attach(const std::shared_ptr<Shared>& shared)
{
    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    {
        const std::scoped_lock lock(shared->mutex);
        if (!shared->alive)
        {
            return;
        }
        shared->manager = manager;
        shared->sessionChanged =
            manager.CurrentSessionChanged(winrt::auto_revoke, [weak = std::weak_ptr(shared)](auto&&, auto&&) {
                if (auto alive = weak.lock())
                {
                    follow(alive);
                }
            });
    }
    follow(shared);
}

template <class Action>
void command(const std::shared_ptr<Shared>& shared, Action action)
{
    background([shared, action] {
        if (auto session = shared->current())
        {
            action(session);
        }
    });
}

} // namespace

MediaWatcher::MediaWatcher(std::function<void(const MediaState&)> changed)
    : m_shared(std::make_shared<Shared>())
{
    m_shared->listener = std::move(changed);
    background([shared = m_shared] { attach(shared); });
}

MediaWatcher::~MediaWatcher()
{
    {
        const std::scoped_lock lock(m_shared->mutex);
        m_shared->alive = false;
        m_shared->listener = nullptr;
    }
    // Unsubscribing talks to the media service too: not on this thread.
    background([shared = m_shared] { shared->detach(); });
}

void MediaWatcher::toggle()
{
    command(m_shared, [](const auto& session) { session.TryTogglePlayPauseAsync(); });
}

void MediaWatcher::play()
{
    command(m_shared, [](const auto& session) { session.TryPlayAsync(); });
}

void MediaWatcher::pause()
{
    command(m_shared, [](const auto& session) { session.TryPauseAsync(); });
}

void MediaWatcher::next()
{
    command(m_shared, [](const auto& session) { session.TrySkipNextAsync(); });
}

void MediaWatcher::previous()
{
    command(m_shared, [](const auto& session) { session.TrySkipPreviousAsync(); });
}

} // namespace rmw::providers
