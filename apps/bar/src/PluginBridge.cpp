#include "PluginBridge.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/Channel.hpp"
#include "rmw/platform/Pipe.hpp"

#include <slint.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace rmw::barapp
{

using namespace std::chrono_literals;

struct PluginBridge::State
{
    std::mutex mutex;
    std::unique_ptr<platform::MessageChannel> channel;
    Handler handle;                  // UI thread
    std::function<void()> connected; // UI thread
    std::atomic<bool> alive{true};
    std::atomic<int> badMessages{0};
};

PluginBridge::PluginBridge(std::wstring pipe, std::filesystem::path hostImage, Handler handle,
                           std::function<void()> connected)
    : m_state(std::make_shared<State>())
{
    m_state->handle = std::move(handle);
    m_state->connected = std::move(connected);
    m_accept = std::jthread([state = m_state, pipe = std::move(pipe),
                             hostImage = std::move(hostImage)](const std::stop_token& stop) {
        auto server = platform::PipeServer::create(pipe);
        if (!server)
        {
            log::error("bar: cannot serve the plugin pipe: {}", server.error().toString());
            return;
        }
        while (!stop.stop_requested())
        {
            auto connection = server->accept(250ms);
            if (!connection)
            {
                continue;
            }
            const auto image = platform::processImagePath(connection->peerProcessId());
            if (!image || !platform::samePath(*image, hostImage))
            {
                log::warn("bar: refused a plugin pipe client that is not rmw-plugin-host");
                continue;
            }
            const std::weak_ptr<State> weak = state;
            auto channel = std::make_unique<platform::MessageChannel>(
                std::move(*connection),
                [weak](std::string text) {
                    auto message = bar::bridge::decode(text);
                    auto owner = weak.lock();
                    if (!owner)
                    {
                        return;
                    }
                    if (!message)
                    {
                        if (owner->badMessages++ < 10)
                        {
                            log::warn("bar: bad message from the plugin host: {}", message.error().message());
                        }
                        return;
                    }
                    slint::invoke_from_event_loop([weak, message = std::move(*message)]() mutable {
                        if (auto alive = weak.lock(); alive && alive->alive)
                        {
                            alive->handle(std::move(message));
                        }
                    });
                },
                [] { log::info("bar: the plugin host went away"); });
            {
                const std::scoped_lock lock(state->mutex);
                state->channel = std::move(channel); // a new host replaces the old one
            }
            log::info("bar: the plugin host connected");
            slint::invoke_from_event_loop([weak] {
                if (auto alive = weak.lock(); alive && alive->alive)
                {
                    alive->connected();
                }
            });
        }
    });
}

PluginBridge::~PluginBridge()
{
    m_state->alive = false;
    m_accept = {};
    const std::scoped_lock lock(m_state->mutex);
    m_state->channel.reset();
}

void PluginBridge::send(const bar::bridge::Message& message)
{
    const std::scoped_lock lock(m_state->mutex);
    if (m_state->channel && m_state->channel->open())
    {
        m_state->channel->send(bar::bridge::encode(message));
    }
}

} // namespace rmw::barapp
