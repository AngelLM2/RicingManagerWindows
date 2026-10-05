#include "MediaWidget.hpp"

#include "rmw/providers/Media.hpp"

#include <memory>
#include <mutex>
#include <string>

// What Windows is playing (providers::MediaWatcher talks to the media service on pool threads).
// Only the resulting text is handed to the bar's loop.
namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

class MediaProvider final : public Provider
{
public:
    MediaProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_instance(instance)
        , m_owner(std::make_shared<Owner>())
        , m_watcher([owner = m_owner](const providers::MediaState& state) { publish(owner, state); })
    {
        m_owner->provider = this;
        show({}, false, false);
        const auto command = [this](void (providers::MediaWatcher::*action)()) {
            return [this, action](std::span<const Value>) {
                (m_watcher.*action)();
                return Value();
            };
        };
        instance->set_callback(m_prefix + "-toggle", command(&providers::MediaWatcher::toggle));
        instance->set_callback(m_prefix + "-next", command(&providers::MediaWatcher::next));
        instance->set_callback(m_prefix + "-previous", command(&providers::MediaWatcher::previous));
    }

    ~MediaProvider() override
    {
        const std::scoped_lock lock(m_owner->mutex);
        m_owner->provider = nullptr;
    }
    MediaProvider(const MediaProvider&) = delete;
    MediaProvider& operator=(const MediaProvider&) = delete;
    MediaProvider(MediaProvider&&) = delete;
    MediaProvider& operator=(MediaProvider&&) = delete;

private:
    struct Owner
    {
        std::mutex mutex;
        MediaProvider* provider = nullptr; // UI thread only, and only while it lives
    };

    // Hands plain values to the UI thread; Slint objects are touched only there.
    static void publish(const std::shared_ptr<Owner>& owner, const providers::MediaState& state)
    {
        std::string title = state.title;
        if (!state.artist.empty())
        {
            title += " — " + state.artist;
        }
        const bool shown = !title.empty();
        slint::invoke_from_event_loop([owner, title = std::move(title), playing = state.playing, shown] {
            MediaProvider* provider = nullptr;
            {
                const std::scoped_lock lock(owner->mutex);
                provider = owner->provider;
            }
            if (provider != nullptr)
            {
                provider->show(title, playing, shown);
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
    std::shared_ptr<Owner> m_owner;
    providers::MediaWatcher m_watcher; // last: stops before the rest goes away
};

} // namespace

std::unique_ptr<Provider> makeMediaProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<MediaProvider>(slot, instance);
}

} // namespace rmw::barapp
