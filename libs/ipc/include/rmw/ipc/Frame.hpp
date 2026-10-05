#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/base/Limits.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Wire format: a 4-byte little-endian length, then that many bytes of UTF-8 JSON
// (docs/03 §8). Messages over the limit close the connection.
namespace rmw::ipc
{

inline constexpr std::size_t kMaxMessageBytes = limits::kIpcMaxMessageBytes;

// Fails with LimitExceeded when `message` is too large to send.
[[nodiscard]] Result<std::vector<std::byte>> encodeFrame(std::string_view message);

// Reassembles frames from a byte stream that arrives in arbitrary pieces.
class FrameDecoder
{
public:
    explicit FrameDecoder(std::size_t maxMessageBytes = kMaxMessageBytes) noexcept
        : m_max(maxMessageBytes)
    {
    }

    // Adds received bytes. After an error the stream is unusable: close the connection.
    [[nodiscard]] Result<void> feed(std::span<const std::byte> bytes);
    // The next complete message, if any.
    [[nodiscard]] std::optional<std::string> next();

private:
    std::size_t m_max;
    std::vector<std::byte> m_buffer;
    std::size_t m_read = 0; // consumed prefix of m_buffer
    bool m_failed = false;
};

} // namespace rmw::ipc
