#include "MediaWidget.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <objbase.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

// What Windows is playing (what shows in the volume flyout's media controls), through the system
// media transport controls. Every WinRT call goes to another process and may take its time, so
// none runs on the UI thread (whose heartbeat the supervisor watches): they run on pool threads,
// and only the resulting text is handed to the bar's loop.
namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

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

class MediaProvider final : public Provider
{
public:
    MediaProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_instance(instance)
        , m_shared(std::make_shared<Shared>())
    {
        m_shared->owner = this;
        show({}, false, false);
        const auto command = [shared = m_shared](auto action) {
            return [shared, action](std::span<const Value>) {
                background([shared, action] {
                    if (auto session = shared->current())
                    {
                        action(session);
                    }
                });
                return Value();
            };
        };
        instance->set_callback(m_prefix + "-toggle",
                               command([](const auto& session) { session.TryTogglePlayPauseAsync(); }));
        instance->set_callback(m_prefix + "-next",
                               command([](const auto& session) { session.TrySkipNextAsync(); }));
        instance->set_callback(m_prefix + "-previous",
                               command([](const auto& session) { session.TrySkipPreviousAsync(); }));
        background([shared = m_shared] { attach(shared); });
    }

    ~MediaProvider() override
    {
        {
            const std::scoped_lock lock(m_shared->mutex);
            m_shared->alive = false;
            m_shared->owner = nullptr;
        }
        // Unsubscribing talks to the media service too: not on this thread.
        background([shared = m_shared] { shared->detach(); });
    }
    MediaProvider(const MediaProvider&) = delete;
    MediaProvider& operator=(const MediaProvider&) = delete;
    MediaProvider(MediaProvider&&) = delete;
    MediaProvider& operator=(MediaProvider&&) = delete;

private:
    struct Shared
    {
        std::mutex mutex;
        bool alive = true;
        MediaProvider* owner = nullptr; // UI thread only, and only while alive
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

    // Pool thread.
    static void attach(const std::shared_ptr<Shared>& shared)
    {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        {
            const std::scoped_lock lock(shared->mutex);
            if (!shared->alive)
            {
                return;
            }
            shared->manager = manager;
            shared->sessionChanged = manager.CurrentSessionChanged(
                winrt::auto_revoke, [weak = std::weak_ptr(shared)](auto&&, auto&&) {
                    if (auto alive = weak.lock())
                    {
                        follow(alive);
                    }
                });
        }
        follow(shared);
    }

    // Pool thread: a player opened, closed or took over.
    static void follow(const std::shared_ptr<Shared>& shared)
    {
        GlobalSystemMediaTransportControlsSession session{nullptr};
        {
            const std::scoped_lock lock(shared->mutex);
            if (!shared->alive || !shared->manager)
            {
                return;
            }
            shared->propertiesChanged.revoke();
            shared->playbackChanged.revoke();
            session = shared->manager.GetCurrentSession();
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
    static void refresh(const std::shared_ptr<Shared>& shared)
    {
        const auto session = shared->current();
        if (!session)
        {
            publish(shared, {}, false, false);
            return;
        }
        const bool playing = session.GetPlaybackInfo().PlaybackStatus() ==
                             GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        const auto properties = session.TryGetMediaPropertiesAsync().get();
        std::string text = winrt::to_string(properties.Title());
        if (const std::string artist = winrt::to_string(properties.Artist()); !artist.empty())
        {
            text += " — " + artist;
        }
        const bool shown = !text.empty();
        publish(shared, std::move(text), playing, shown);
    }

    // Hands plain values to the UI thread; Slint objects are touched only there.
    static void publish(const std::shared_ptr<Shared>& shared, std::string title, bool playing, bool shown)
    {
        slint::invoke_from_event_loop([shared, title = std::move(title), playing, shown] {
            MediaProvider* owner = nullptr;
            {
                const std::scoped_lock lock(shared->mutex);
                owner = shared->alive ? shared->owner : nullptr;
            }
            if (owner != nullptr)
            {
                owner->show(title, playing, shown);
            }
        });
    }

    void show(const std::string& title, bool playing, bool shown)
    {
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-title", Value(slint::SharedString(title)));
            (*instance)->set_property(m_prefix + "-playing", Value(playing));
            (*instance)->set_property(m_prefix + "-shown", Value(shown));
        }
    }

    std::string m_prefix;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    std::shared_ptr<Shared> m_shared;
};

} // namespace

std::unique_ptr<Provider> makeMediaProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<MediaProvider>(slot, instance);
}

} // namespace rmw::barapp
