#include "rmw/ipc/JsonRpc.hpp"

#include "rmw/ipc/Frame.hpp"

#include <format>

namespace rmw::ipc
{
namespace
{

constexpr std::size_t kMaxMethodBytes = 64;

json::ParseLimits messageLimits()
{
    json::ParseLimits limits;
    limits.maxBytes = kMaxMessageBytes;
    return limits;
}

bool validId(const Json& id)
{
    return id.is_null() || id.is_number_integer() || id.is_string();
}

std::optional<ErrorCode> errorCodeFromName(std::string_view name)
{
    for (int value = static_cast<int>(ErrorCode::Unknown); value <= static_cast<int>(ErrorCode::Database);
         ++value)
    {
        const auto code = static_cast<ErrorCode>(value);
        if (toString(code) == name)
        {
            return code;
        }
    }
    return std::nullopt;
}

} // namespace

Result<Request> parseRequest(std::string_view text)
{
    auto message = json::parse(text, messageLimits());
    if (!message)
    {
        return std::unexpected(std::move(message.error()));
    }
    if (!message->is_object() || json::stringOr(*message, "jsonrpc", "") != "2.0")
    {
        return fail(ErrorCode::InvalidData, "not a JSON-RPC 2.0 message");
    }
    Request request;
    if (const auto id = message->find("id"); id != message->end())
    {
        if (!validId(*id))
        {
            return fail(ErrorCode::InvalidData, "request id must be a number or a string");
        }
        request.id = *id;
    }
    const auto method = message->find("method");
    if (method == message->end() || !method->is_string() || method->get_ref<const std::string&>().empty() ||
        method->get_ref<const std::string&>().size() > kMaxMethodBytes)
    {
        return fail(ErrorCode::InvalidData, "request without a valid method");
    }
    request.method = method->get<std::string>();
    if (const auto params = message->find("params"); params != message->end())
    {
        if (!params->is_object())
        {
            return fail(ErrorCode::InvalidData, "params must be an object");
        }
        request.params = *params;
    }
    return request;
}

Result<Response> parseResponse(std::string_view text)
{
    auto message = json::parse(text, messageLimits());
    if (!message)
    {
        return std::unexpected(std::move(message.error()));
    }
    if (!message->is_object() || json::stringOr(*message, "jsonrpc", "") != "2.0" ||
        !message->contains("id") || !validId((*message)["id"]))
    {
        return fail(ErrorCode::InvalidData, "not a JSON-RPC 2.0 response");
    }
    Response response;
    response.id = (*message)["id"];
    if (const auto error = message->find("error"); error != message->end())
    {
        if (!error->is_object() || !error->contains("code") || !(*error)["code"].is_number_integer())
        {
            return fail(ErrorCode::InvalidData, "malformed JSON-RPC error");
        }
        RpcError rpc;
        rpc.code = (*error)["code"].get<int>();
        rpc.message = json::stringOr(*error, "message", "");
        rpc.data = json::memberOr(*error, "data", nullptr);
        response.error = std::move(rpc);
        return response;
    }
    const auto result = message->find("result");
    if (result == message->end())
    {
        return fail(ErrorCode::InvalidData, "response has neither result nor error");
    }
    response.result = *result;
    return response;
}

std::string requestText(std::int64_t id, std::string_view method, const Json& params)
{
    return json::dump(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
}

std::string notificationText(std::string_view method, const Json& params)
{
    return json::dump(Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

std::string resultText(const Json& id, const Json& result)
{
    return json::dump(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

std::string errorText(const Json& id, const RpcError& error)
{
    Json body{{"code", error.code}, {"message", error.message}};
    if (!error.data.is_null())
    {
        body["data"] = error.data;
    }
    return json::dump(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(body)}});
}

RpcError rpcError(RpcCode code, std::string message)
{
    return {static_cast<int>(code), std::move(message), nullptr};
}

RpcError toRpcError(const Error& error)
{
    return {static_cast<int>(RpcCode::OperationFailed), error.message(),
            Json{{"code", toString(error.code())}, {"native", error.nativeCode()}}};
}

Error fromRpcError(const RpcError& error)
{
    ErrorCode code = ErrorCode::Unknown;
    if (error.code == static_cast<int>(RpcCode::Busy))
    {
        code = ErrorCode::Busy;
    }
    else if (error.code == static_cast<int>(RpcCode::Incompatible))
    {
        code = ErrorCode::Unsupported;
    }
    else if (error.data.is_object() && error.data.contains("code") && error.data["code"].is_string())
    {
        code = errorCodeFromName(error.data["code"].get<std::string>()).value_or(ErrorCode::Unknown);
    }
    const std::int64_t native =
        error.data.is_object() && error.data.contains("native") && error.data["native"].is_number_integer()
            ? error.data["native"].get<std::int64_t>()
            : 0;
    return {code, error.message, native};
}

Json helloParams(std::string_view buildId)
{
    return Json{{"protoMajor", kProtocolMajor}, {"protoMinor", kProtocolMinor}, {"buildId", buildId}};
}

Result<void> checkHello(const Json& hello)
{
    const json::Reader reader(hello, "hello");
    auto major = reader.integer("protoMajor");
    if (!major)
    {
        return std::unexpected(std::move(major.error()));
    }
    if (*major != kProtocolMajor)
    {
        return fail(ErrorCode::Unsupported,
                    std::format("protocol {} is not compatible with {} (another version of "
                                "Ricing Manager is running)",
                                *major, kProtocolMajor));
    }
    return {};
}

} // namespace rmw::ipc
