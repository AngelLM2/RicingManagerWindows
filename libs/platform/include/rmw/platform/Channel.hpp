#pragma once

#include "rmw/platform/Pipe.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rmw::platform
{

// A connected pipe whose messages are read and written on threads of their own, so neither side's
// loop ever blocks on the other (the bar and the plugin host talk this way, docs/12 §3).
// `received` runs on the reading thread for every message; `closed` runs once, on whichever
// thread notices first, when the peer goes away or a write times out (a stuck peer).
class MessageChannel
{
public:
    using Received = std::function<void(std::string message)>;
    using Closed = std::function<void()>;

    MessageChannel(PipeConnection connection, Received received, Closed closed);
    ~MessageChannel(); // stops both threads; `closed` is not called for that
    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;
    MessageChannel(MessageChannel&&) = delete;
    MessageChannel& operator=(MessageChannel&&) = delete;

    // Queued; false when closed, or when more than kMaxQueuedBytes wait already (dropped).
    bool send(std::string message);
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] std::uint32_t peerProcessId() const noexcept { return m_peer; }

    static constexpr std::size_t kMaxQueuedBytes = 4u << 20;

    struct State;

private:
    std::shared_ptr<State> m_state;
    std::uint32_t m_peer = 0;
    std::jthread m_reader;
    std::jthread m_writer;
};

} // namespace rmw::platform
