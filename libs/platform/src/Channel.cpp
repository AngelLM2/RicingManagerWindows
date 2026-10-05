#include "rmw/platform/Channel.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace rmw::platform
{

using namespace std::chrono_literals;

struct MessageChannel::State
{
    PipeConnection connection;
    Received received;
    Closed closed;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::size_t queuedBytes = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> ended{false};

    // Once: the peer went away (not when we stop on purpose).
    void end()
    {
        if (!ended.exchange(true) && !stopping)
        {
            closed();
        }
        wake.notify_all();
    }
};

MessageChannel::MessageChannel(PipeConnection connection, Received received, Closed closed)
    : m_state(std::make_shared<State>())
    , m_peer(connection.peerProcessId())
{
    m_state->connection = std::move(connection);
    m_state->received = std::move(received);
    m_state->closed = std::move(closed);
    m_reader = std::jthread([state = m_state] {
        while (!state->stopping && !state->ended)
        {
            auto message = state->connection.receive(500ms);
            if (message)
            {
                state->received(std::move(*message));
            }
            else if (message.error().code() != ErrorCode::Timeout)
            {
                state->end();
            }
        }
    });
    m_writer = std::jthread([state = m_state] {
        while (true)
        {
            std::string message;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock,
                                 [&] { return state->stopping || state->ended || !state->queue.empty(); });
                if (state->stopping || state->ended)
                {
                    return;
                }
                message = std::move(state->queue.front());
                state->queue.pop_front();
                state->queuedBytes -= message.size();
            }
            if (!state->connection.send(message, 5s))
            {
                state->end();
                return;
            }
        }
    });
}

MessageChannel::~MessageChannel()
{
    m_state->stopping = true;
    m_state->wake.notify_all();
    m_state->connection.cancel();
    m_reader = {};
    m_writer = {};
}

bool MessageChannel::send(std::string message)
{
    {
        const std::scoped_lock lock(m_state->mutex);
        if (m_state->ended || m_state->queuedBytes + message.size() > kMaxQueuedBytes)
        {
            return false;
        }
        m_state->queuedBytes += message.size();
        m_state->queue.push_back(std::move(message));
    }
    m_state->wake.notify_all();
    return true;
}

bool MessageChannel::open() const noexcept
{
    return !m_state->ended;
}

} // namespace rmw::platform
