#include "rmw/ipc/Frame.hpp"

#include <format>

namespace rmw::ipc
{
namespace
{

constexpr std::size_t kHeader = 4;

std::size_t readLength(std::span<const std::byte> header) noexcept
{
    return std::to_integer<std::size_t>(header[0]) | (std::to_integer<std::size_t>(header[1]) << 8) |
           (std::to_integer<std::size_t>(header[2]) << 16) | (std::to_integer<std::size_t>(header[3]) << 24);
}

} // namespace

Result<std::vector<std::byte>> encodeFrame(std::string_view message)
{
    if (message.size() > kMaxMessageBytes)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("message too large ({} bytes, limit {})", message.size(), kMaxMessageBytes));
    }
    std::vector<std::byte> frame(kHeader + message.size());
    const auto length = static_cast<std::uint32_t>(message.size());
    for (std::size_t i = 0; i < kHeader; ++i)
    {
        frame[i] = std::byte((length >> (8 * i)) & 0xFF);
    }
    for (std::size_t i = 0; i < message.size(); ++i)
    {
        frame[kHeader + i] = std::byte(static_cast<unsigned char>(message[i]));
    }
    return frame;
}

Result<void> FrameDecoder::feed(std::span<const std::byte> bytes)
{
    if (m_failed)
    {
        return fail(ErrorCode::InvalidData, "the connection already sent an invalid frame");
    }
    // Compact before growing so a long-lived connection does not accumulate consumed bytes.
    if (m_read > 0 && m_read == m_buffer.size())
    {
        m_buffer.clear();
        m_read = 0;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());

    // Validate the pending header as soon as it is complete: never buffer an oversize message.
    if (m_buffer.size() - m_read >= kHeader)
    {
        const std::size_t length = readLength(std::span(m_buffer).subspan(m_read, kHeader));
        if (length > m_max)
        {
            m_failed = true;
            return fail(ErrorCode::LimitExceeded,
                        std::format("incoming message too large ({} bytes)", length));
        }
    }
    if (m_buffer.size() - m_read > m_max + kHeader + kHeader)
    {
        m_failed = true; // more than one full message queued without being read
        return fail(ErrorCode::LimitExceeded, "too much unread data");
    }
    return {};
}

std::optional<std::string> FrameDecoder::next()
{
    if (m_failed || m_buffer.size() - m_read < kHeader)
    {
        return std::nullopt;
    }
    const std::size_t length = readLength(std::span(m_buffer).subspan(m_read, kHeader));
    if (length > m_max)
    {
        m_failed = true;
        return std::nullopt;
    }
    if (m_buffer.size() - m_read - kHeader < length)
    {
        return std::nullopt;
    }
    const auto* data = reinterpret_cast<const char*>(m_buffer.data() + m_read + kHeader);
    std::string message(data, length);
    m_read += kHeader + length;
    if (m_read == m_buffer.size())
    {
        m_buffer.clear();
        m_read = 0;
    }
    else if (m_read > m_max)
    {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_read));
        m_read = 0;
    }
    return message;
}

} // namespace rmw::ipc
