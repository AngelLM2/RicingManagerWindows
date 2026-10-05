#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Value.hpp"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

// rmw-bar <-> rmw-plugin-host (docs/12 §3: the bar never runs plugin code; it gets values and sends
// clicks). Framed JSON over a pipe the core names (the bar serves it). Every message is checked on
// arrival like any input: the host runs code we did not write.
namespace rmw::bar::bridge
{

inline constexpr int kVersion = 1;

struct Hello
{
    int version = kVersion;
};

struct InstanceInfo
{
    std::string key;    // "<bar>/<slot>": "0/w3"
    std::string plugin; // the plugin id
    domain::Value config;

    friend bool operator==(const InstanceInfo&, const InstanceInfo&) = default;
};

// bar -> host: every plugin widget the bars show now (after each rebuild).
struct Instances
{
    std::vector<InstanceInfo> list;
};

// host -> bar: new values for some properties of one widget.
struct Set
{
    std::string key;
    domain::Value properties; // a table
};

// bar -> host: a widget callback fired.
struct Event
{
    std::string key;
    std::string callback;
    domain::Value::Array arguments;
};

// host -> bar: something only the bar can do (it got the click, so Windows lets it bring a window
// forward): "window.activate", "window.focus", "window.minimize", "window.close" with
// { id = <window> }.
struct Action
{
    std::string name;
    domain::Value arguments;
};

using Message = std::variant<Hello, Instances, Set, Event, Action>;

[[nodiscard]] std::string encode(const Message& message);
// The JSON limits apply; unknown kinds and wrong shapes are errors.
[[nodiscard]] Result<Message> decode(std::string_view text);

} // namespace rmw::bar::bridge
