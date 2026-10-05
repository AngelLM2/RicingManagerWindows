#include "rmw/bar/Bridge.hpp"

#include "rmw/domain/Identifiers.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/json/Value.hpp"

#include <format>

namespace rmw::bar::bridge
{
namespace
{

using json::Json;

constexpr std::size_t kMaxInstances = 256;
constexpr std::size_t kMaxKeyBytes = 64;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxArguments = 8;
constexpr std::size_t kMaxWatched = 8;

Result<std::string> text(const Json& object, std::string_view key, std::size_t maxBytes)
{
    const Json value = json::memberOr(object, key, Json());
    if (!value.is_string() || value.get<std::string>().empty() || value.get<std::string>().size() > maxBytes)
    {
        return fail(ErrorCode::InvalidData, std::format("bridge: '{}' is missing or too long", key));
    }
    return value.get<std::string>();
}

Result<domain::Value> value(const Json& object, std::string_view key)
{
    return json::toValue(json::memberOr(object, key, Json()));
}

} // namespace

std::string encode(const Message& message)
{
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Hello>)
            {
                return json::dump(Json{{"t", "hello"}, {"v", m.version}});
            }
            else if constexpr (std::is_same_v<T, Instances>)
            {
                Json list = Json::array();
                for (const auto& instance : m.list)
                {
                    list.push_back({{"key", instance.key},
                                    {"plugin", instance.plugin},
                                    {"config", json::fromValue(instance.config)}});
                }
                return json::dump(Json{{"t", "instances"}, {"list", std::move(list)}});
            }
            else if constexpr (std::is_same_v<T, Set>)
            {
                return json::dump(
                    Json{{"t", "set"}, {"key", m.key}, {"props", json::fromValue(m.properties)}});
            }
            else if constexpr (std::is_same_v<T, Event>)
            {
                return json::dump(Json{{"t", "event"},
                                       {"key", m.key},
                                       {"callback", m.callback},
                                       {"args", json::fromValue(domain::Value{m.arguments})}});
            }
            else if constexpr (std::is_same_v<T, Action>)
            {
                return json::dump(
                    Json{{"t", "action"}, {"name", m.name}, {"args", json::fromValue(m.arguments)}});
            }
            else if constexpr (std::is_same_v<T, Watch>)
            {
                return json::dump(Json{{"t", "watch"}, {"providers", m.providers}});
            }
            else
            {
                return json::dump(
                    Json{{"t", "provide"}, {"name", m.name}, {"value", json::fromValue(m.value)}});
            }
        },
        message);
}

bool isBarProvider(std::string_view name) noexcept
{
    return name == "tray";
}

Result<Message> decode(std::string_view input)
{
    auto parsed = json::parse(input);
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    if (!root.is_object())
    {
        return fail(ErrorCode::InvalidData, "bridge: expected an object");
    }
    const std::string kind = json::stringOr(root, "t", "");
    if (kind == "hello")
    {
        return Hello{static_cast<int>(json::intOr(root, "v", 0))};
    }
    if (kind == "instances")
    {
        const Json list = json::memberOr(root, "list", Json());
        if (!list.is_array() || list.size() > kMaxInstances)
        {
            return fail(ErrorCode::InvalidData, "bridge: instances: expected a list");
        }
        Instances instances;
        for (const auto& item : list)
        {
            auto key = text(item, "key", kMaxKeyBytes);
            auto plugin = text(item, "plugin", 128);
            auto config = value(item, "config");
            if (!key || !plugin || !config || !domain::isValidPackageId(*plugin))
            {
                return fail(ErrorCode::InvalidData, "bridge: instances: invalid entry");
            }
            instances.list.push_back({std::move(*key), std::move(*plugin), std::move(*config)});
        }
        return instances;
    }
    if (kind == "set")
    {
        auto key = text(root, "key", kMaxKeyBytes);
        auto properties = value(root, "props");
        if (!key || !properties || properties->table() == nullptr)
        {
            return fail(ErrorCode::InvalidData, "bridge: set: expected a key and a table of properties");
        }
        return Set{std::move(*key), std::move(*properties)};
    }
    if (kind == "event")
    {
        auto key = text(root, "key", kMaxKeyBytes);
        auto callback = text(root, "callback", kMaxNameBytes);
        auto arguments = value(root, "args");
        const auto* list = arguments ? std::get_if<domain::Value::Array>(&arguments->data) : nullptr;
        const bool none = arguments && (arguments->isNull() ||
                                        (arguments->table() != nullptr && arguments->table()->empty()));
        if (!key || !callback || (list == nullptr && !none) ||
            (list != nullptr && list->size() > kMaxArguments))
        {
            return fail(ErrorCode::InvalidData, "bridge: event: invalid");
        }
        return Event{std::move(*key), std::move(*callback), list != nullptr ? *list : domain::Value::Array{}};
    }
    if (kind == "action")
    {
        auto name = text(root, "name", kMaxNameBytes);
        auto arguments = value(root, "args");
        if (!name || !arguments)
        {
            return fail(ErrorCode::InvalidData, "bridge: action: invalid");
        }
        return Action{std::move(*name), std::move(*arguments)};
    }
    if (kind == "watch")
    {
        const Json list = json::memberOr(root, "providers", Json());
        if (!list.is_array() || list.size() > kMaxWatched)
        {
            return fail(ErrorCode::InvalidData, "bridge: watch: expected a list of providers");
        }
        Watch watch;
        for (const auto& item : list)
        {
            if (!item.is_string() || !isBarProvider(item.get<std::string>()))
            {
                return fail(ErrorCode::InvalidData, "bridge: watch: not a provider of the bar");
            }
            watch.providers.push_back(item.get<std::string>());
        }
        return watch;
    }
    if (kind == "provide")
    {
        auto name = text(root, "name", kMaxNameBytes);
        auto provided = value(root, "value");
        if (!name || !provided || !isBarProvider(*name))
        {
            return fail(ErrorCode::InvalidData, "bridge: provide: invalid");
        }
        return Provide{std::move(*name), std::move(*provided)};
    }
    return fail(ErrorCode::InvalidData, std::format("bridge: unknown message '{}'", kind));
}

} // namespace rmw::bar::bridge
