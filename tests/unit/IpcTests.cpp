#include "rmw/ipc/Frame.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace
{

using namespace rmw::ipc;
using namespace std::chrono_literals;

std::string randomPipeName()
{
    return "rmw-test-" + rmw::platform::randomHex(8).value();
}

std::wstring widePipe(const std::string& suffix)
{
    return LR"(\\.\pipe\)" + std::wstring(suffix.begin(), suffix.end());
}

TEST(Frame, RoundTripsAcrossArbitrarySplits)
{
    std::vector<std::byte> stream;
    for (const char* message : {"{\"a\":1}", "", "{\"b\":\"夜景\"}"})
    {
        const auto frame = encodeFrame(message).value();
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    for (const std::size_t chunk : {1u, 3u, 7u, 1000u})
    {
        FrameDecoder decoder;
        std::vector<std::string> messages;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk)
        {
            ASSERT_TRUE(
                decoder.feed(std::span(stream).subspan(offset, std::min(chunk, stream.size() - offset))));
            while (auto message = decoder.next())
            {
                messages.push_back(*message);
            }
        }
        EXPECT_EQ(messages, (std::vector<std::string>{"{\"a\":1}", "", "{\"b\":\"夜景\"}"}))
            << "chunk " << chunk;
    }
}

TEST(Frame, RefusesOversizeMessagesBeforeBufferingThem)
{
    FrameDecoder decoder(16);
    const std::vector<std::byte> header = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                           std::byte{0x7F}};
    EXPECT_FALSE(decoder.feed(header).has_value());
    EXPECT_FALSE(decoder.feed(header).has_value()); // stays failed
    EXPECT_FALSE(decoder.next().has_value());
    EXPECT_FALSE(encodeFrame(std::string(kMaxMessageBytes + 1, 'x')).has_value());
}

TEST(JsonRpc, ParsesRequestsAndResponses)
{
    const auto request = parseRequest(requestText(7, "rice.apply", {{"path", "C:\\x"}}));
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->id, 7);
    EXPECT_EQ(request->method, "rice.apply");
    EXPECT_EQ(request->params["path"], "C:\\x");

    const auto notification = parseRequest(notificationText("rice.applied", Json::object()));
    ASSERT_TRUE(notification.has_value());
    EXPECT_TRUE(notification->id.is_null());

    const auto ok = parseResponse(resultText(7, {{"done", true}}));
    ASSERT_TRUE(ok.has_value());
    EXPECT_FALSE(ok->error.has_value());
    EXPECT_EQ(ok->result["done"], true);

    const auto failed =
        parseResponse(errorText(7, toRpcError(rmw::Error(rmw::ErrorCode::NotFound, "no rice"))));
    ASSERT_TRUE(failed.has_value());
    ASSERT_TRUE(failed->error.has_value());
    const rmw::Error back = fromRpcError(*failed->error);
    EXPECT_EQ(back.code(), rmw::ErrorCode::NotFound);
    EXPECT_EQ(back.message(), "no rice");
    EXPECT_EQ(fromRpcError(rpcError(RpcCode::Busy, "busy")).code(), rmw::ErrorCode::Busy);
}

TEST(JsonRpc, RejectsMalformedMessagesWithoutThrowing)
{
    for (const char* text :
         {"", "[]", "{}", R"({"jsonrpc":2,"method":"x"})", R"({"jsonrpc":"2.0"})",
          R"({"jsonrpc":"2.0","method":5})", R"({"jsonrpc":"2.0","method":"x","params":[1]})",
          R"({"jsonrpc":"2.0","method":"x","id":{"a":1}})"})
    {
        EXPECT_FALSE(parseRequest(text).has_value()) << text;
    }
    for (const char* text : {R"({"jsonrpc":"2.0"})", R"({"jsonrpc":"2.0","id":1})",
                             R"({"jsonrpc":"2.0","id":1,"error":{"code":"x"}})",
                             R"({"jsonrpc":"2.0","id":1,"error":{"code":1,"message":5}})"})
    {
        const auto response = parseResponse(text);
        if (response.has_value())
        {
            EXPECT_TRUE(response->error.has_value()) << text; // tolerated: a message of the wrong type
        }
    }
}

TEST(JsonRpc, HelloRequiresTheSameMajorVersion)
{
    EXPECT_TRUE(checkHello(helloParams("test")).has_value());
    EXPECT_EQ(checkHello({{"protoMajor", kProtocolMajor + 1}}).error().code(), rmw::ErrorCode::Unsupported);
    EXPECT_FALSE(checkHello({{"protoMajor", "1"}}).has_value());
}

TEST(Pipe, ServerAndClientExchangeMessages)
{
    const std::wstring name = widePipe(randomPipeName());
    auto server = rmw::platform::PipeServer::create(name);
    ASSERT_TRUE(server.has_value()) << server.error().toString();
    const auto self = rmw::platform::currentExecutable().value();

    std::thread client([&] {
        auto connection = rmw::platform::connectPipe(name, 5s, self);
        ASSERT_TRUE(connection.has_value()) << connection.error().toString();
        ASSERT_TRUE(connection->send(requestText(1, "hello", helloParams("t")), 5s));
        const auto reply = connection->receive(5s);
        ASSERT_TRUE(reply.has_value()) << reply.error().toString();
        EXPECT_EQ(parseResponse(*reply)->result["ok"], true);
    });

    auto connection = server->accept(5s);
    ASSERT_TRUE(connection.has_value()) << connection.error().toString();
    EXPECT_EQ(connection->peerProcessId(), GetCurrentProcessId());
    const auto message = connection->receive(5s);
    ASSERT_TRUE(message.has_value()) << message.error().toString();
    EXPECT_EQ(parseRequest(*message)->method, "hello");
    ASSERT_TRUE(connection->send(resultText(1, {{"ok", true}}), 5s));
    client.join();

    // After the client is gone, reading reports the disconnection.
    EXPECT_EQ(connection->receive(2s).error().code(), rmw::ErrorCode::NotFound);
}

TEST(Pipe, NamesAreOwnedAndServersAreChecked)
{
    const std::wstring name = widePipe(randomPipeName());
    auto server = rmw::platform::PipeServer::create(name);
    ASSERT_TRUE(server.has_value());
    // A second process (or object) cannot take over the name.
    EXPECT_EQ(rmw::platform::PipeServer::create(name).error().code(), rmw::ErrorCode::AlreadyExists);

    // A client that expects another program refuses to talk to this server.
    std::thread accepter([&] { [[maybe_unused]] const auto accepted = server->accept(3s); });
    const auto wrong = rmw::platform::connectPipe(name, 3s, L"C:\\Windows\\notepad.exe");
    accepter.join();
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().code(), rmw::ErrorCode::AccessDenied);

    EXPECT_EQ(rmw::platform::connectPipe(widePipe(randomPipeName()), 1s, L"x").error().code(),
              rmw::ErrorCode::NotFound);
}

// The review of v1.7: a client that connected and left before accept() left the instance failing
// every accept after it, at once (the core's accept loop spun).
TEST(Pipe, AClientThatLeftBeforeItWasAcceptedDoesNotBreakTheServer)
{
    const std::wstring name = widePipe(randomPipeName());
    auto server = rmw::platform::PipeServer::create(name);
    ASSERT_TRUE(server.has_value()) << server.error().toString();
    const auto self = rmw::platform::currentExecutable().value();
    {
        auto gone = rmw::platform::connectPipe(name, 5s, self);
        ASSERT_TRUE(gone.has_value()) << gone.error().toString();
    }
    const auto left = server->accept(500ms);
    ASSERT_FALSE(left.has_value());
    EXPECT_EQ(left.error().code(), rmw::ErrorCode::Timeout) << left.error().toString();

    std::thread client([&] {
        auto connection = rmw::platform::connectPipe(name, 5s, self);
        ASSERT_TRUE(connection.has_value()) << connection.error().toString();
        ASSERT_TRUE(connection->send(requestText(1, "hello", helloParams("t")), 5s));
    });
    auto connection = server->accept(5s);
    ASSERT_TRUE(connection.has_value()) << connection.error().toString();
    const auto message = connection->receive(5s);
    ASSERT_TRUE(message.has_value()) << message.error().toString();
    EXPECT_EQ(parseRequest(*message)->method, "hello");
    client.join();
}

TEST(Pipe, TimeoutsAreReported)
{
    const std::wstring name = widePipe(randomPipeName());
    auto server = rmw::platform::PipeServer::create(name);
    ASSERT_TRUE(server.has_value());
    EXPECT_EQ(server->accept(100ms).error().code(), rmw::ErrorCode::Timeout);

    std::thread client([&] {
        auto connection = rmw::platform::connectPipe(name, 5s, rmw::platform::currentExecutable().value());
        ASSERT_TRUE(connection.has_value());
        std::this_thread::sleep_for(500ms); // say nothing for a while
    });
    auto connection = server->accept(5s);
    ASSERT_TRUE(connection.has_value());
    EXPECT_EQ(connection->receive(100ms).error().code(), rmw::ErrorCode::Timeout);
    client.join();
}

// The lifecycle review (v1.8.1): a stopping core kept its pipe until it ended; a client connected,
// waited 10 s for a hello that never came, and said "busy".
TEST(Pipe, AStoppingServerIsGoneAtOnce)
{
    const std::wstring name = widePipe(randomPipeName());
    auto server = rmw::platform::PipeServer::create(name);
    ASSERT_TRUE(server.has_value());
    // Its accept ends when asked, not at its timeout.
    HANDLE stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    ASSERT_NE(stop, nullptr);
    const auto asked = std::chrono::steady_clock::now();
    EXPECT_EQ(server->accept(10s, stop).error().code(), rmw::ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - asked, 2s);
    CloseHandle(stop);
    // Closed, nobody serves the name: a client knows at once (and starts the next core).
    server->close();
    const auto self = rmw::platform::currentExecutable().value();
    EXPECT_EQ(rmw::platform::connectPipe(name, 1s, self).error().code(), rmw::ErrorCode::NotFound);
    EXPECT_EQ(server->accept(100ms).error().code(), rmw::ErrorCode::Cancelled);
    // And the name can be served again (by the next core).
    EXPECT_TRUE(rmw::platform::PipeServer::create(name).has_value());
}

// A core started while the last one stops waits for it instead of leaving at once.
TEST(SingleInstance, ASecondOneWaitsForTheFirstToEnd)
{
    const std::string token = rmw::platform::randomHex(8).value();
    const std::wstring purpose = L"test-" + std::wstring(token.begin(), token.end());
    std::promise<void> held;
    std::promise<void> letGo;
    std::thread first([&] {
        auto instance = rmw::platform::SingleInstance::acquire(purpose);
        EXPECT_TRUE(instance && instance->owned());
        held.set_value();
        letGo.get_future().wait();
    }); // (the instance ends with the thread)
    held.get_future().wait();
    auto second = rmw::platform::SingleInstance::acquire(purpose);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->owned());
    EXPECT_FALSE(second->waitOwned(100ms)); // the first still holds it
    letGo.set_value();
    EXPECT_TRUE(second->waitOwned(5s));
    EXPECT_TRUE(second->owned());
    first.join();
}

} // namespace
