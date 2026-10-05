// The core's pipe: frames arriving in any pieces, then JSON-RPC requests and replies (docs/03 §8,
// docs/06 §4). The first byte sets the size of the pieces.

#include "rmw/ipc/Frame.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/json/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace
{

void readMessage(std::string_view message)
{
    [[maybe_unused]] const auto request = rmw::ipc::parseRequest(message);
    [[maybe_unused]] const auto response = rmw::ipc::parseResponse(message);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    const std::size_t piece = 1 + (data[0] % 64);
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data) + 1, size - 1);
    rmw::ipc::FrameDecoder decoder(64 * 1024);
    for (std::size_t at = 0; at < bytes.size(); at += piece)
    {
        if (!decoder.feed(bytes.subspan(at, std::min(piece, bytes.size() - at))))
        {
            break;
        }
        while (auto message = decoder.next())
        {
            readMessage(*message);
        }
    }
    // The same bytes as one message, and as JSON.
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    readMessage(text);
    if (auto value = rmw::json::parse(text))
    {
        [[maybe_unused]] const auto dumped = rmw::json::dump(*value);
    }
    return 0;
}
