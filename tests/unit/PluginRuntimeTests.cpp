#include "rmw/domain/PluginManifest.hpp"
#include "rmw/plugins/Runtime.hpp"
#include "rmw/plugins/Url.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace
{

using namespace rmw;
using namespace std::chrono_literals;
using plugins::Clock;
using plugins::LogLevel;

struct FakeHost final : plugins::IPluginHost
{
    struct Action
    {
        std::string plugin;
        std::string name;
        domain::Value args;
        bool byUser = false;
    };

    domain::Value provider(std::string_view name) override
    {
        if (name == "system.cpu")
        {
            return domain::Value{domain::Value::Table{{"usage", domain::Value{12.5}}}};
        }
        return {};
    }
    void watchProviders(std::span<const std::string> names) override
    {
        watched.assign(names.begin(), names.end());
    }
    void action(const std::string& plugin, const std::string& name, const domain::Value& args,
                bool byUser) override
    {
        actions.push_back({plugin, name, args, byUser});
    }
    void setWidget(const std::string& instance, const domain::Value& properties) override
    {
        widgets.emplace_back(instance, properties);
    }
    void http(plugins::HttpRequest request) override { requests.push_back(std::move(request)); }
    void log(const std::string& plugin, LogLevel level, std::string_view text) override
    {
        logs.push_back(std::format("{}|{}|{}", plugin, static_cast<int>(level), text));
    }
    void entering(const std::string& plugin) override { current = plugin; }
    void left() override { current.clear(); }
    void failed(const std::string& plugin, std::string_view reason) override
    {
        failures.push_back(std::format("{}|{}", plugin, reason));
    }

    [[nodiscard]] bool logged(std::string_view text) const
    {
        return std::ranges::any_of(
            logs, [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
    [[nodiscard]] std::string allLogs() const
    {
        std::string text;
        for (const auto& line : logs)
        {
            text += line + "\n";
        }
        return text;
    }

    std::vector<std::string> watched;
    std::vector<Action> actions;
    std::vector<std::pair<std::string, domain::Value>> widgets;
    std::vector<plugins::HttpRequest> requests;
    std::vector<std::string> logs;
    std::vector<std::string> failures;
    std::string current;
};

constexpr std::string_view kManifest = R"toml(
schema = 1
[plugin]
id = "io.github.test.{0}"
name = "{0}"
version = "1.0.0"
api = "^1.0"
kind = ["bar.widget", "action"]
[permissions]
providers = ["system.cpu", "windows.list"]
actions = ["bar.toggle", "media.control"]
network = ["api.example.com"]
storage = true
[entry]
script = "main.luau"
[[action]]
name = "focus"
[widget]
ui = "widget.slint"
component = "W"
[widget.structs.Entry]
title = "string"
icon = "image"
[widget.properties]
text = "string"
count = "int"
items = "[Entry]"
icon = "image"
[widget.callbacks]
clicked = []
picked = ["int"]
)toml";

plugins::PluginSpec spec(std::string_view name, std::string source, bool grantAll = true)
{
    domain::Diagnostics diagnostics;
    auto manifest =
        domain::parsePluginManifest(std::vformat(kManifest, std::make_format_args(name)), diagnostics, {});
    EXPECT_TRUE(manifest.has_value()) << (manifest ? "" : manifest.error().message());
    plugins::PluginSpec result;
    result.manifest = *manifest;
    result.granted = grantAll ? manifest->permissions : domain::PluginPermissions{};
    result.source = std::move(source);
    result.config = domain::Value{domain::Value::Table{{"greeting", domain::Value{std::string("hi")}}}};
    return result;
}

const domain::Value* member(const domain::Value& value, std::string_view key)
{
    return value.find(key);
}

class PluginRuntime : public ::testing::Test
{
protected:
    FakeHost host;
    plugins::Runtime runtime{host};
    Clock::time_point now = Clock::time_point{} + 1h;

    void load(std::string_view name, std::string source, bool grantAll = true)
    {
        auto loaded = runtime.load(spec(name, std::move(source), grantAll), now);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message() << "\n" << host.allLogs();
    }

    void advance(std::chrono::milliseconds by)
    {
        now += by;
        runtime.tick(now);
    }
};

TEST_F(PluginRuntime, ScriptsRunTimersAndFeedWidgets)
{
    load("clock", R"lua(
local ticks = 0
rmw.widget(function(w)
    w:set({ text = rmw.config.greeting, count = 0 })
    w:every(1000, function()
        ticks += 1
        w:set("count", ticks)
    end)
    w:on("clicked", function()
        w:set("text", "clicked")
        rmw.action("bar.toggle")
    end)
end)
)lua");
    const std::vector<plugins::InstanceSpec> instances{{"bar0/w1", "io.github.test.clock", {}}};
    runtime.setInstances(instances, now);
    ASSERT_EQ(host.widgets.size(), 1u) << host.allLogs();
    EXPECT_EQ(host.widgets[0].first, "bar0/w1");
    EXPECT_EQ(*member(host.widgets[0].second, "text"), domain::Value{std::string("hi")});

    advance(1000ms);
    ASSERT_EQ(host.widgets.size(), 2u);
    EXPECT_EQ(*member(host.widgets[1].second, "count"), domain::Value{std::int64_t{1}});

    // A click answers at once, even right after an update, and calls the action as the user.
    runtime.widgetEvent("bar0/w1", "clicked", {}, now + 10ms);
    ASSERT_EQ(host.widgets.size(), 3u);
    EXPECT_EQ(*member(host.widgets[2].second, "text"), domain::Value{std::string("clicked")});
    ASSERT_EQ(host.actions.size(), 1u);
    EXPECT_EQ(host.actions[0].name, "bar.toggle");
    EXPECT_TRUE(host.actions[0].byUser);
    EXPECT_TRUE(host.failures.empty());
}

TEST_F(PluginRuntime, UpdatesAreRateLimited)
{
    load("busy", R"lua(
rmw.widget(function(w)
    local n = 0
    w:every(250, function() n += 1; w:set("count", n) end)
    rmw.every(250, function() n += 1; w:set("count", n) end)
end)
)lua");
    const std::vector<plugins::InstanceSpec> instances{{"k", "io.github.test.busy", {}}};
    runtime.setInstances(instances, now);
    for (int i = 0; i < 8; ++i)
    {
        advance(250ms);
    }
    // Two timers set it every 250 ms, but at most one update per 250 ms leaves.
    EXPECT_LE(host.widgets.size(), 9u);
    EXPECT_GE(host.widgets.size(), 7u);
}

TEST_F(PluginRuntime, AnEndlessLoopIsStoppedAndThePluginDisabledAfterThreeTimes)
{
    load("loop", R"lua(
rmw.every(250, function() while true do end end)
)lua");
    load("good", R"lua(
rmw.every(250, function() rmw.log("alive") end)
)lua");
    const auto started = Clock::now();
    for (int i = 0; i < 4; ++i)
    {
        advance(250ms);
    }
    const auto spent = Clock::now() - started;
    EXPECT_TRUE(host.logged("ran longer than 50 ms")) << host.allLogs();
    ASSERT_EQ(host.failures.size(), 1u) << host.allLogs();
    EXPECT_NE(host.failures[0].find("io.github.test.loop"), std::string::npos);
    EXPECT_LT(spent, 2s); // three budgets of 50 ms, not forever
    // The other plugin runs on.
    EXPECT_TRUE(host.logged("io.github.test.good|0|alive"));
    const auto status = runtime.status();
    const auto loop =
        std::ranges::find(status, std::string("io.github.test.loop"), &plugins::PluginStatus::id);
    ASSERT_NE(loop, status.end());
    EXPECT_TRUE(loop->failed);
    EXPECT_EQ(loop->memoryBytes, 0u); // its VM is gone
}

TEST_F(PluginRuntime, AMemoryBombHitsTheCap)
{
    load("bomb", R"lua(
local hoard = {}
rmw.every(250, function()
    for i = 1, 1e7 do hoard[#hoard + 1] = string.rep("x", 1048576) .. i end
end)
)lua");
    load("good", R"lua(rmw.every(250, function() rmw.log("alive") end))lua");
    advance(250ms);
    EXPECT_TRUE(host.logged("memory limit of 16 MB")) << host.allLogs();
    EXPECT_TRUE(host.logged("alive"));
    for (const auto& status : runtime.status())
    {
        EXPECT_LE(status.memoryBytes, limits::kPluginVmMaxBytes);
    }
}

TEST_F(PluginRuntime, ALoadThatNeverEndsFails)
{
    const auto loaded = runtime.load(spec("stuck", "while true do end"), now);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message().find("ran longer"), std::string::npos) << loaded.error().message();
    EXPECT_FALSE(runtime.load(spec("broken", "this is not lua"), now).has_value());
}

TEST_F(PluginRuntime, TheSandboxHoldsAndPermissionsAreChecked)
{
    load("probe", R"lua(
assert(loadstring == nil and getfenv == nil and setfenv == nil and debug == nil)
assert(io == nil and package == nil and os.execute == nil and os.getenv == nil and require == nil)
assert(not pcall(function() string.upper = nil end), "string is read-only")
assert(not pcall(function() rmw.log = nil end), "rmw is read-only")
rmw.onaction("focus", function(args) rmw.log("focus " .. tostring(args.level)) end)
)lua",
         false); // nothing approved

    // Each forbidden call is an error in the plugin, logged; three of them stop it.
    const std::vector<std::string> attempts = {
        R"(rmw.provider("system.cpu"))",
        R"(rmw.action("bar.toggle"))",
        R"(rmw.storage.set("a", 1))",
        R"(rmw.http.get("https://api.example.com/x", function() end))",
    };
    for (std::size_t i = 0; i < attempts.size(); ++i)
    {
        FakeHost local;
        plugins::Runtime isolated(local);
        auto loaded = isolated.load(spec(std::format("p{}", i), attempts[i], false), now);
        ASSERT_FALSE(loaded.has_value()) << attempts[i];
        EXPECT_NE(loaded.error().message().find("permission denied"), std::string::npos)
            << loaded.error().message();
        EXPECT_TRUE(local.actions.empty());
        EXPECT_TRUE(local.requests.empty());
    }
    // Asking for something the manifest never declared: denied even if "approved".
    auto widened = spec("wide", R"(rmw.provider("media"))");
    widened.granted.providers.emplace_back("media");
    const auto loaded = runtime.load(std::move(widened), now);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message().find("permission denied"), std::string::npos);

    // A declared action runs with its arguments.
    domain::Value args{domain::Value::Table{{"level", domain::Value{std::int64_t{2}}}}};
    ASSERT_TRUE(runtime.runAction("io.github.test.probe", "focus", args, true, now).has_value())
        << host.allLogs();
    EXPECT_TRUE(host.logged("focus 2"));
    EXPECT_FALSE(runtime.runAction("io.github.test.probe", "other", {}, true, now).has_value());
}

TEST_F(PluginRuntime, WidgetValuesAreCheckedAgainstTheManifest)
{
    load("types", R"lua(
rmw.widget(function(w)
    local ok1 = pcall(function() w:set("count", "not a number") end)
    local ok2 = pcall(function() w:set("missing", 1) end)
    local ok3 = pcall(function() w:set("items", { { title = "a", extra = 1 } }) end)
    local ok4 = pcall(function() w:set("icon", "C:/Windows/x.png") end)
    local ok5 = pcall(function() w:on("nope", function() end) end)
    rmw.log(string.format("%s %s %s %s %s", tostring(ok1), tostring(ok2), tostring(ok3), tostring(ok4), tostring(ok5)))
    w:set({ items = { { title = "a", icon = "window:42" } }, icon = "icons/a.png" })
end)
)lua");
    const std::vector<plugins::InstanceSpec> instances{{"k", "io.github.test.types", {}}};
    runtime.setInstances(instances, now);
    EXPECT_TRUE(host.logged("false false false false false")) << host.allLogs();
    ASSERT_EQ(host.widgets.size(), 1u) << host.allLogs();
    const auto* items = member(host.widgets[0].second, "items");
    ASSERT_NE(items, nullptr);
    ASSERT_TRUE(std::holds_alternative<domain::Value::Array>(items->data));
}

TEST_F(PluginRuntime, StorageIsKeptAndCapped)
{
    load("store", R"lua(
rmw.storage.set("count", (rmw.storage.get("count") or 0) + 1)
rmw.storage.set("list", { 1, 2, 3 })
rmw.storage.remove("list")
local full = pcall(function() rmw.storage.set("big", string.rep("x", 2 * 1024 * 1024)) end)
rmw.log("full " .. tostring(full))
)lua");
    EXPECT_TRUE(host.logged("full false")) << host.allLogs();
    auto dirty = runtime.takeDirtyStorage();
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(*member(dirty[0].second, "count"), domain::Value{std::int64_t{1}});
    EXPECT_EQ(member(dirty[0].second, "list"), nullptr);
    EXPECT_TRUE(runtime.takeDirtyStorage().empty());

    // Next start: the saved table comes back.
    auto again = spec("store", R"lua(rmw.log("count " .. tostring(rmw.storage.get("count"))))lua");
    again.storage = dirty[0].second;
    ASSERT_TRUE(runtime.load(std::move(again), now).has_value());
    EXPECT_TRUE(host.logged("count 1"));
}

TEST_F(PluginRuntime, HttpGoesThroughTheHostToDeclaredDomainsOnly)
{
    load("net", R"lua(
rmw.http.get("https://API.example.com/v1/weather?city=Lisboa#frag", { headers = { Accept = "application/json" } },
    function(r) rmw.log("first " .. r.status .. " " .. tostring(r.ok) .. " " .. r.body) end)
rmw.http.get("https://api.example.com/again", function(r) rmw.log("second " .. tostring(r.error)) end)
local blocked = pcall(function() rmw.http.get("https://evil.example.org/", function() end) end)
local plain = pcall(function() rmw.http.get("http://api.example.com/", function() end) end)
local header = pcall(function() rmw.http.get("https://api.example.com/", { headers = { Cookie = "a" } }, function() end) end)
rmw.log(string.format("blocked %s %s %s", tostring(blocked), tostring(plain), tostring(header)))
)lua");
    EXPECT_TRUE(host.logged("blocked false false false")) << host.allLogs();
    ASSERT_EQ(host.requests.size(), 1u);
    EXPECT_EQ(host.requests[0].host, "api.example.com");
    EXPECT_EQ(host.requests[0].target, "/v1/weather?city=Lisboa");
    ASSERT_EQ(host.requests[0].headers.size(), 1u);

    plugins::HttpResponse response;
    response.status = 200;
    response.body = R"({"temp": 21})";
    runtime.httpDone("io.github.test.net", host.requests[0].id, response, now);
    EXPECT_TRUE(host.logged(R"(first 200 true {"temp": 21})")) << host.allLogs();
    advance(10ms); // the second one is answered on the next turn: too soon after the first
    EXPECT_TRUE(host.logged("second too many requests")) << host.allLogs();
}

TEST_F(PluginRuntime, JsonAndProviders)
{
    load("data", R"lua(
local t = rmw.json.decode('{"a": [1, 2, {"b": true}], "c": "d"}')
assert(t.a[3].b == true and t.c == "d")
local text = rmw.json.encode({ x = 1, y = { "z" } })
assert(text == '{"x":1,"y":["z"]}', text)
local bad, why = rmw.json.decode("{nope")
assert(bad == nil and why ~= nil)
assert(rmw.provider("system.cpu").usage == 12.5)
rmw.on("system.cpu", function(cpu) rmw.log("cpu " .. cpu.usage) end)
)lua");
    EXPECT_EQ(host.watched, std::vector<std::string>{"system.cpu"});
    runtime.providerChanged("system.cpu", domain::Value{domain::Value::Table{{"usage", domain::Value{50.0}}}},
                            now);
    EXPECT_TRUE(host.logged("cpu 50")) << host.allLogs();
    runtime.unload("io.github.test.data");
    EXPECT_TRUE(host.watched.empty());
}

TEST_F(PluginRuntime, InstancesComeAndGo)
{
    load("life", R"lua(
rmw.widget(function(w)
    rmw.log("open " .. w.id .. " " .. tostring(w.config.city))
    w:every(500, function() rmw.log("tick " .. w.id) end)
    w:onclose(function() rmw.log("close " .. w.id) end)
end)
)lua");
    const domain::Value lisbon{domain::Value::Table{{"city", domain::Value{std::string("Lisboa")}}}};
    std::vector<plugins::InstanceSpec> instances{{"a", "io.github.test.life", lisbon},
                                                 {"b", "io.github.test.life", {}}};
    runtime.setInstances(instances, now);
    EXPECT_TRUE(host.logged("open a Lisboa"));
    EXPECT_TRUE(host.logged("open b nil"));
    instances.pop_back();
    runtime.setInstances(instances, now);
    EXPECT_TRUE(host.logged("close b"));
    host.logs.clear();
    advance(500ms);
    EXPECT_TRUE(host.logged("tick a"));
    EXPECT_FALSE(host.logged("tick b")); // its timers went with it

    // Reloading the plugin (a saved script) opens its widgets again.
    load("life", R"lua(rmw.widget(function(w) rmw.log("again " .. w.id) end))lua");
    EXPECT_TRUE(host.logged("again a"));
}

TEST(PluginUrl, OnlyPlainHttpsAddresses)
{
    const auto ok = plugins::parseHttpsUrl("HTTPS://Api.Example.com:443?x=1");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->host, "api.example.com");
    EXPECT_EQ(ok->target, "/?x=1");
    for (const std::string_view url :
         {"http://a.com/", "https://user:pw@a.com/", "https://a.com:8443/", "https://a.com/ x",
          "https://[::1]/", "https://a.com\\@b.com/", "ftp://a.com", "https://", "https://a.com/\xC3\xA9"})
    {
        EXPECT_FALSE(plugins::parseHttpsUrl(url).has_value()) << url;
    }
    EXPECT_TRUE(plugins::checkHttpHeader("Accept", "application/json").has_value());
    EXPECT_FALSE(plugins::checkHttpHeader("Host", "x").has_value());
    EXPECT_FALSE(plugins::checkHttpHeader("Proxy-Foo", "x").has_value());
    EXPECT_FALSE(plugins::checkHttpHeader("X-A", "a\r\nInjected: 1").has_value());
}

} // namespace
