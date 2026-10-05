#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// JSON-RPC 2.0 messages (docs/03 §8). Every connection starts with "hello"; a peer with another
// major protocol version is refused, so an old core left running after an update is replaced.
namespace rmw::ipc
{

using json::Json;

inline constexpr int kProtocolMajor = 1;
inline constexpr int kProtocolMinor = 0;

enum class RpcCode : int
{
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    Busy = -32001,            // another operation is running
    Incompatible = -32002,    // protocol major version differs
    OperationFailed = -32003, // the method ran and failed; data.code names the rmw::ErrorCode
};

struct Request
{
    Json id; // null for notifications
    std::string method;
    Json params = Json::object();
};

struct RpcError
{
    int code = static_cast<int>(RpcCode::InternalError);
    std::string message;
    Json data;
};

struct Response
{
    Json id;
    Json result;
    std::optional<RpcError> error;
};

[[nodiscard]] Result<Request> parseRequest(std::string_view text);
[[nodiscard]] Result<Response> parseResponse(std::string_view text);

[[nodiscard]] std::string requestText(std::int64_t id, std::string_view method, const Json& params);
[[nodiscard]] std::string notificationText(std::string_view method, const Json& params);
[[nodiscard]] std::string resultText(const Json& id, const Json& result);
[[nodiscard]] std::string errorText(const Json& id, const RpcError& error);
[[nodiscard]] RpcError rpcError(RpcCode code, std::string message);

// An rmw::Error as OperationFailed with data {"code": "<ErrorCode name>"}, and back.
[[nodiscard]] RpcError toRpcError(const Error& error);
[[nodiscard]] Error fromRpcError(const RpcError& error);

// Handshake.
[[nodiscard]] Json helloParams(std::string_view buildId);
// Checks the peer's hello (params or result): same major version required.
[[nodiscard]] Result<void> checkHello(const Json& hello);

} // namespace rmw::ipc
