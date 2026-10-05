#include "rmw/plugins/Runtime.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/json/Value.hpp"
#include "rmw/plugins/Url.hpp"

#include <luau/lua.h>
#include <luau/luacode.h>
#include <luau/lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>

namespace rmw::plugins
{
namespace
{

constexpr std::chrono::milliseconds kCallbackBudget{limits::kPluginCallbackMaxMs};
constexpr std::chrono::milliseconds kLoadBudget{200};
constexpr std::size_t kMaxTimers = 64;
constexpr std::size_t kMaxHandlers = 64;
constexpr std::size_t kMaxInFlightHttp = 4;
constexpr std::size_t kHttpBodyMaxBytes = 64u << 10;
constexpr std::size_t kMaxHeaders = 16;
constexpr int kValueMaxDepth = 16;
constexpr std::size_t kValueMaxElements = 10'000;
constexpr std::size_t kValueTextMaxBytes = 64u << 10;
constexpr std::size_t kLogLineMaxBytes = 1'024;
constexpr std::size_t kLogBurst = 50;
constexpr std::chrono::seconds kLogWindow{10};
constexpr std::size_t kStorageKeyMaxBytes = 128;
constexpr double kMaxTimerMs = 24.0 * 60 * 60 * 1000;

// Each VM's allocator, interrupt deadline and memory use.
struct Vm
{
    std::size_t bytes = 0;
    std::size_t limit = limits::kPluginVmMaxBytes;
    bool capHit = false;
    bool timedOut = false;
    Clock::time_point deadline{};
    lua_State* L = nullptr;
    lua_State* T = nullptr; // the script's own thread (its globals live there)

    Vm() = default;
    ~Vm()
    {
        if (L != nullptr)
        {
            lua_close(L);
        }
    }
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&) = delete;
    Vm& operator=(Vm&&) = delete;
};

void* allocate(void* data, void* block, std::size_t oldSize, std::size_t newSize)
{
    auto* vm = static_cast<Vm*>(data);
    if (newSize == 0)
    {
        vm->bytes -= oldSize;
        std::free(block); // NOLINT(cppcoreguidelines-no-malloc): Luau's allocator contract
        return nullptr;
    }
    if (newSize > oldSize && vm->bytes + (newSize - oldSize) > vm->limit)
    {
        vm->capHit = true;
        return nullptr; // Luau raises "not enough memory" in the script
    }
    void* grown = std::realloc(block, newSize); // NOLINT(cppcoreguidelines-no-malloc)
    if (grown != nullptr)
    {
        vm->bytes = vm->bytes - oldSize + newSize;
    }
    return grown;
}

// Luau calls this at loop back-edges and calls: a callback past its deadline is ended.
void interrupt(lua_State* L, int gc)
{
    if (gc >= 0)
    {
        return;
    }
    auto* vm = static_cast<Vm*>(lua_callbacks(L)->userdata);
    if (Clock::now() > vm->deadline)
    {
        vm->timedOut = true;
        luaL_error(L, "the callback ran longer than %d ms", static_cast<int>(kCallbackBudget.count()));
    }
}

struct Timer
{
    std::uint64_t id = 0;
    Clock::time_point due{};
    std::chrono::milliseconds period{0}; // 0: once
    int function = LUA_NOREF;
    std::string instance; // empty: the plugin's own
};

int absolute(lua_State* L, int index)
{
    return index > 0 || index <= LUA_REGISTRYINDEX ? index : lua_gettop(L) + index + 1;
}

void pushValue(lua_State* L, const domain::Value& value, int depth = 0)
{
    if (depth > kValueMaxDepth)
    {
        lua_pushnil(L);
        return;
    }
    luaL_checkstack(L, 3, "value nested too deeply");
    std::visit(
        [&](const auto& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                lua_pushnil(L);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                lua_pushboolean(L, data ? 1 : 0);
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                lua_pushnumber(L, static_cast<double>(data));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                lua_pushnumber(L, data);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                lua_pushlstring(L, data.data(), data.size());
            }
            else if constexpr (std::is_same_v<T, domain::Value::Array>)
            {
                lua_createtable(L, static_cast<int>(data.size()), 0);
                for (std::size_t i = 0; i < data.size(); ++i)
                {
                    pushValue(L, data[i], depth + 1);
                    lua_rawseti(L, -2, static_cast<int>(i + 1));
                }
            }
            else if constexpr (std::is_same_v<T, domain::Value::Table>)
            {
                lua_createtable(L, 0, static_cast<int>(data.size()));
                for (const auto& member : data)
                {
                    pushValue(L, member.value, depth + 1);
                    lua_setfield(L, -2, member.key.c_str());
                }
            }
        },
        value.data);
}

// A Lua value as data that can leave the VM. Raises a Lua error for what cannot (functions,
// cycles, keys that are not text, invalid UTF-8, too much of it).
domain::Value toValue(lua_State* L, int index, int depth, std::size_t& budget)
{
    index = absolute(L, index);
    if (depth > kValueMaxDepth)
    {
        luaL_error(L, "value nested more than %d levels", kValueMaxDepth);
    }
    if (budget == 0)
    {
        luaL_error(L, "value has more than %d elements", static_cast<int>(kValueMaxElements));
    }
    --budget;
    switch (lua_type(L, index))
    {
    case LUA_TNIL:
        return {};
    case LUA_TBOOLEAN:
        return domain::Value{lua_toboolean(L, index) != 0};
    case LUA_TNUMBER: {
        const double number = lua_tonumber(L, index);
        if (!std::isfinite(number))
        {
            luaL_error(L, "nan and inf cannot leave the plugin");
        }
        constexpr double kExact = 9'007'199'254'740'992.0; // 2^53
        if (std::trunc(number) == number && std::abs(number) <= kExact)
        {
            return domain::Value{static_cast<std::int64_t>(number)};
        }
        return domain::Value{number};
    }
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        const std::string_view view(text, length);
        if (length > kValueTextMaxBytes)
        {
            luaL_error(L, "text longer than %d bytes", static_cast<int>(kValueTextMaxBytes));
        }
        if (!isValidUtf8(view))
        {
            luaL_error(L, "text is not valid UTF-8");
        }
        return domain::Value{std::string(view)};
    }
    case LUA_TTABLE: {
        luaL_checkstack(L, 4, "value nested too deeply");
        const int count = lua_objlen(L, index);
        domain::Value::Array items;
        domain::Value::Table members;
        int entries = 0;
        bool keyed = false;
        lua_pushnil(L);
        while (lua_next(L, index) != 0)
        {
            ++entries;
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                keyed = true;
                std::size_t length = 0;
                const char* key = lua_tolstring(L, -2, &length);
                if (!isValidUtf8(std::string_view(key, length)) || length > kStorageKeyMaxBytes)
                {
                    luaL_error(L, "invalid table key");
                }
                members.push_back({std::string(key, length), toValue(L, -1, depth + 1, budget)});
            }
            else if (lua_type(L, -2) != LUA_TNUMBER)
            {
                luaL_error(L, "table keys are text or list positions");
            }
            lua_pop(L, 1);
        }
        if (keyed)
        {
            if (entries != static_cast<int>(members.size()))
            {
                luaL_error(L, "a table mixes a list and named fields");
            }
            std::ranges::sort(members, {}, &domain::ValueMember::key);
            return domain::Value{std::move(members)};
        }
        if (entries != count)
        {
            luaL_error(L, "a list has holes");
        }
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i)
        {
            lua_rawgeti(L, index, i);
            items.push_back(toValue(L, -1, depth + 1, budget));
            lua_pop(L, 1);
        }
        if (items.empty())
        {
            return domain::Value{domain::Value::Table{}};
        }
        return domain::Value{std::move(items)};
    }
    default:
        break;
    }
    luaL_error(L, "a %s cannot leave the plugin", luaL_typename(L, index));
}

domain::Value toValue(lua_State* L, int index)
{
    std::size_t budget = kValueMaxElements;
    return toValue(L, index, 0, budget);
}

std::string_view checkedString(lua_State* L, int index)
{
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return {text, length};
}

std::chrono::milliseconds checkedDelay(lua_State* L, int index, std::chrono::milliseconds minimum)
{
    const double ms = luaL_checknumber(L, index);
    if (!std::isfinite(ms) || ms < static_cast<double>(minimum.count()) || ms > kMaxTimerMs)
    {
        luaL_error(L, "the interval is at least %d ms", static_cast<int>(minimum.count()));
    }
    return std::chrono::milliseconds(static_cast<std::int64_t>(ms));
}

bool contains(const std::vector<std::string>& list, std::string_view name)
{
    return std::ranges::find(list, name) != list.end();
}

} // namespace

struct Runtime::Instance
{
    std::string key;
    domain::Value config;
    int object = LUA_NOREF;
    std::vector<std::pair<std::string, int>> handlers;
    int onClose = LUA_NOREF;
    domain::Value::Table pending;
    Clock::time_point lastFlush{};
    bool urgent = false;
    bool closed = false;
};

struct Runtime::Plugin
{
    Runtime* runtime = nullptr;
    std::string id;
    PluginSpec spec;
    std::unique_ptr<Vm> vm;
    int widgetFactory = LUA_NOREF;
    std::vector<std::pair<std::string, int>> actionHandlers;
    std::vector<std::pair<std::string, int>> providerHandlers;
    std::vector<Timer> timers;
    std::uint64_t nextTimer = 1;
    std::vector<std::unique_ptr<Instance>> instances;
    domain::Value::Table storage;
    bool storageDirty = false;
    std::uint64_t nextHttp = 1;
    std::vector<std::pair<std::uint64_t, int>> httpCallbacks;
    std::vector<std::pair<std::uint64_t, HttpResponse>> lateResponses;
    std::optional<Clock::time_point> lastHttp;
    std::deque<Clock::time_point> failures;
    std::deque<Clock::time_point> logs;
    bool failed = false;
    bool byUser = false;

    [[nodiscard]] Instance* instance(std::string_view key) const
    {
        for (const auto& item : instances)
        {
            if (item->key == key && !item->closed)
            {
                return item.get();
            }
        }
        return nullptr;
    }

    void unref(int reference) const
    {
        if (vm != nullptr && reference != LUA_NOREF && reference != LUA_REFNIL)
        {
            lua_unref(vm->L, reference);
        }
    }
};

// The rmw.* functions. Each C closure carries its Plugin as upvalue 1 (and a widget's key as
// upvalue 2): a script cannot reach upvalues, so it cannot reach another plugin's data.
struct Api
{
    using Plugin = Runtime::Plugin;

    static Plugin& self(lua_State* L)
    {
        return *static_cast<Plugin*>(lua_touserdata(L, lua_upvalueindex(1)));
    }

    static void push(lua_State* L, Plugin& plugin, lua_CFunction function, const char* name)
    {
        lua_pushlightuserdata(L, &plugin);
        lua_pushcclosure(L, function, name, 1);
    }

    static void field(lua_State* L, Plugin& plugin, lua_CFunction function, const char* name)
    {
        push(L, plugin, function, name);
        lua_setfield(L, -2, name);
    }

    [[noreturn]] static void denied(lua_State* L, std::string_view what)
    {
        luaL_error(
            L,
            "permission denied: %s (add it to [permissions] in plugin.toml; the user approves it at install)",
            std::string(what).c_str());
    }

    // ---- log ----------------------------------------------------------------------------------

    static int write(lua_State* L, LogLevel level)
    {
        Plugin& plugin = self(L);
        const auto now = plugin.runtime->m_now;
        while (!plugin.logs.empty() && now - plugin.logs.front() > kLogWindow)
        {
            plugin.logs.pop_front();
        }
        if (plugin.logs.size() >= kLogBurst)
        {
            return 0; // a chatty plugin does not flood the log
        }
        plugin.logs.push_back(now);
        std::string line;
        const int count = lua_gettop(L);
        for (int i = 1; i <= count && line.size() < kLogLineMaxBytes; ++i)
        {
            std::size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            line += i > 1 ? "\t" : "";
            line.append(text, length);
            lua_pop(L, 1);
        }
        if (line.size() > kLogLineMaxBytes)
        {
            line.resize(kLogLineMaxBytes);
            line += "…";
        }
        plugin.runtime->m_host.log(plugin.id, level, utf16To8Lossy(utf8To16Lossy(line)));
        return 0;
    }

    static int log(lua_State* L) { return write(L, LogLevel::Info); }
    static int warn(lua_State* L) { return write(L, LogLevel::Warning); }

    // ---- timers -------------------------------------------------------------------------------

    static int addTimer(lua_State* L, Plugin& plugin, int first, std::chrono::milliseconds minimum,
                        bool repeat, std::string instance)
    {
        const auto delay = checkedDelay(L, first, minimum);
        luaL_checktype(L, first + 1, LUA_TFUNCTION);
        if (plugin.timers.size() >= kMaxTimers)
        {
            luaL_error(L, "a plugin has at most %d timers", static_cast<int>(kMaxTimers));
        }
        Timer timer;
        timer.id = plugin.nextTimer++;
        timer.due = plugin.runtime->m_now + delay;
        timer.period = repeat ? delay : std::chrono::milliseconds(0);
        timer.function = lua_ref(L, first + 1);
        timer.instance = std::move(instance);
        plugin.timers.push_back(timer);
        lua_pushnumber(L, static_cast<double>(timer.id));
        return 1;
    }

    static int every(lua_State* L) { return addTimer(L, self(L), 1, kMinRepeatInterval, true, {}); }
    static int after(lua_State* L) { return addTimer(L, self(L), 1, kMinDelay, false, {}); }

    static int cancel(lua_State* L)
    {
        Plugin& plugin = self(L);
        const auto id = static_cast<std::uint64_t>(luaL_checknumber(L, 1));
        const auto found = std::ranges::find(plugin.timers, id, &Timer::id);
        if (found != plugin.timers.end())
        {
            plugin.unref(found->function);
            plugin.timers.erase(found);
        }
        return 0;
    }

    // ---- providers and actions ----------------------------------------------------------------

    static std::string checkedProvider(lua_State* L, const Plugin& plugin)
    {
        const std::string name(checkedString(L, 1));
        if (domain::findPluginProvider(name) == nullptr)
        {
            luaL_error(L, "unknown provider '%s'", name.c_str());
        }
        if (!contains(plugin.spec.granted.providers, name))
        {
            denied(L, std::format("provider '{}'", name));
        }
        return name;
    }

    static int provider(lua_State* L)
    {
        Plugin& plugin = self(L);
        const std::string name = checkedProvider(L, plugin);
        pushValue(L, plugin.runtime->m_host.provider(name));
        return 1;
    }

    static int on(lua_State* L)
    {
        Plugin& plugin = self(L);
        const std::string name = checkedProvider(L, plugin);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (plugin.providerHandlers.size() >= kMaxHandlers)
        {
            luaL_error(L, "too many handlers");
        }
        plugin.providerHandlers.emplace_back(name, lua_ref(L, 2));
        plugin.runtime->updateWatches();
        return 0;
    }

    static int action(lua_State* L)
    {
        Plugin& plugin = self(L);
        const std::string name(checkedString(L, 1));
        if (domain::findPluginAction(name) == nullptr)
        {
            luaL_error(L, "unknown action '%s'", name.c_str());
        }
        if (!contains(plugin.spec.granted.actions, name))
        {
            denied(L, std::format("action '{}'", name));
        }
        domain::Value arguments;
        if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        {
            luaL_checktype(L, 2, LUA_TTABLE);
            arguments = toValue(L, 2);
        }
        plugin.runtime->m_host.action(plugin.id, name, arguments, plugin.byUser);
        return 0;
    }

    static int onAction(lua_State* L)
    {
        Plugin& plugin = self(L);
        const std::string name(checkedString(L, 1));
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (plugin.spec.manifest.findAction(name) == nullptr)
        {
            luaL_error(L, "action '%s' is not declared in plugin.toml ([[action]])", name.c_str());
        }
        const auto found =
            std::ranges::find(plugin.actionHandlers, name, &std::pair<std::string, int>::first);
        if (found != plugin.actionHandlers.end())
        {
            plugin.unref(found->second);
            found->second = lua_ref(L, 2);
        }
        else
        {
            plugin.actionHandlers.emplace_back(name, lua_ref(L, 2));
        }
        return 0;
    }

    // ---- widgets --------------------------------------------------------------------------------

    static int widget(lua_State* L)
    {
        Plugin& plugin = self(L);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        if (!plugin.spec.manifest.widget)
        {
            luaL_error(L, "rmw.widget is for bar.widget plugins");
        }
        plugin.unref(plugin.widgetFactory);
        plugin.widgetFactory = lua_ref(L, 1);
        return 0;
    }

    static Runtime::Instance* instanceOf(lua_State* L, Plugin& plugin)
    {
        std::size_t length = 0;
        const char* key = lua_tolstring(L, lua_upvalueindex(2), &length);
        return plugin.instance(std::string_view(key, length));
    }

    static void setProperty(lua_State* L, Plugin& plugin, Runtime::Instance& instance, std::string_view name,
                            int valueIndex)
    {
        const auto& widget = *plugin.spec.manifest.widget;
        const auto property = std::ranges::find(widget.properties, name, &domain::WidgetProperty::name);
        if (property == widget.properties.end())
        {
            luaL_error(L, "the widget has no property '%s' ([widget.properties] in plugin.toml)",
                       std::string(name).c_str());
        }
        domain::Value value = toValue(L, valueIndex);
        if (auto checked = domain::checkWidgetValue(widget, property->type, value); !checked)
        {
            luaL_error(L, "%s: %s", std::string(name).c_str(), checked.error().message().c_str());
        }
        checkImages(L, plugin, widget, property->type, value);
        const auto found = std::ranges::find(instance.pending, name, &domain::ValueMember::key);
        if (found != instance.pending.end())
        {
            found->value = std::move(value);
        }
        else
        {
            instance.pending.push_back({std::string(name), std::move(value)});
        }
    }

    // Every image a value holds, in lists and structs too.
    static void checkImages(lua_State* L, const Plugin& plugin, const domain::PluginWidget& widget,
                            const domain::WidgetTypeRef& type, const domain::Value& value)
    {
        if (type.array)
        {
            if (const auto* items = std::get_if<domain::Value::Array>(&value.data))
            {
                domain::WidgetTypeRef item = type;
                item.array = false;
                for (const auto& element : *items)
                {
                    checkImages(L, plugin, widget, item, element);
                }
            }
            return;
        }
        if (!type.structName.empty())
        {
            const auto* declared = widget.findStruct(type.structName);
            const auto* members = value.table();
            if (declared == nullptr || members == nullptr)
            {
                return;
            }
            for (const auto& member : *members)
            {
                const auto field =
                    std::ranges::find(declared->fields, member.key, &domain::WidgetField::name);
                const auto* text = std::get_if<std::string>(&member.value.data);
                if (field != declared->fields.end() && field->type == domain::WidgetType::Image &&
                    text != nullptr)
                {
                    checkImageSource(L, plugin, *text);
                }
            }
            return;
        }
        if (const auto* text = std::get_if<std::string>(&value.data);
            text != nullptr && type.type == domain::WidgetType::Image)
        {
            checkImageSource(L, plugin, *text);
        }
    }

    // A window's icon tells what the user has open: the same permission as the window list.
    static void checkImageSource(lua_State* L, const Plugin& plugin, std::string_view image)
    {
        if (image.starts_with("window:") && !contains(plugin.spec.granted.providers, "windows.list"))
        {
            denied(L, "window icons need provider 'windows.list'");
        }
    }

    static int widgetSet(lua_State* L)
    {
        Plugin& plugin = self(L);
        Runtime::Instance* instance = instanceOf(L, plugin);
        if (instance == nullptr)
        {
            return 0; // closed meanwhile
        }
        if (lua_type(L, 2) == LUA_TTABLE)
        {
            lua_pushnil(L);
            while (lua_next(L, 2) != 0)
            {
                if (lua_type(L, -2) != LUA_TSTRING)
                {
                    luaL_error(L, "w:set{...} takes property = value pairs");
                }
                std::size_t length = 0;
                const char* name = lua_tolstring(L, -2, &length);
                setProperty(L, plugin, *instance, std::string_view(name, length), -1);
                lua_pop(L, 1);
            }
            return 0;
        }
        const std::string name(checkedString(L, 2));
        setProperty(L, plugin, *instance, name, 3);
        return 0;
    }

    static int widgetOn(lua_State* L)
    {
        Plugin& plugin = self(L);
        Runtime::Instance* instance = instanceOf(L, plugin);
        const std::string name(checkedString(L, 2));
        luaL_checktype(L, 3, LUA_TFUNCTION);
        const auto& callbacks = plugin.spec.manifest.widget->callbacks;
        if (std::ranges::find(callbacks, name, &domain::WidgetCallback::name) == callbacks.end())
        {
            luaL_error(L, "the widget has no callback '%s' ([widget.callbacks] in plugin.toml)",
                       name.c_str());
        }
        if (instance == nullptr)
        {
            return 0;
        }
        const auto found = std::ranges::find(instance->handlers, name, &std::pair<std::string, int>::first);
        if (found != instance->handlers.end())
        {
            plugin.unref(found->second);
            found->second = lua_ref(L, 3);
        }
        else
        {
            instance->handlers.emplace_back(name, lua_ref(L, 3));
        }
        return 0;
    }

    static int widgetTimer(lua_State* L, bool repeat)
    {
        Plugin& plugin = self(L);
        Runtime::Instance* instance = instanceOf(L, plugin);
        if (instance == nullptr)
        {
            lua_pushnil(L);
            return 1;
        }
        return addTimer(L, plugin, 2, repeat ? kMinRepeatInterval : kMinDelay, repeat, instance->key);
    }

    static int widgetEvery(lua_State* L) { return widgetTimer(L, true); }
    static int widgetAfter(lua_State* L) { return widgetTimer(L, false); }

    static int widgetOnClose(lua_State* L)
    {
        Plugin& plugin = self(L);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (Runtime::Instance* instance = instanceOf(L, plugin))
        {
            plugin.unref(instance->onClose);
            instance->onClose = lua_ref(L, 2);
        }
        return 0;
    }

    static void pushWidget(lua_State* L, Plugin& plugin, const Runtime::Instance& instance)
    {
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, instance.key.data(), instance.key.size());
        lua_setfield(L, -2, "id");
        pushValue(L, instance.config.isNull() ? domain::Value{domain::Value::Table{}} : instance.config);
        lua_setfield(L, -2, "config");
        const std::pair<lua_CFunction, const char*> methods[] = {
            {widgetSet, "set"},     {widgetOn, "on"},           {widgetEvery, "every"},
            {widgetAfter, "after"}, {widgetOnClose, "onclose"},
        };
        for (const auto& [function, name] : methods)
        {
            lua_pushlightuserdata(L, &plugin);
            lua_pushlstring(L, instance.key.data(), instance.key.size());
            lua_pushcclosure(L, function, name, 2);
            lua_setfield(L, -2, name);
        }
    }

    // ---- storage ------------------------------------------------------------------------------

    static void needStorage(lua_State* L, const Plugin& plugin)
    {
        if (!plugin.spec.granted.storage)
        {
            denied(L, "storage");
        }
    }

    static int storageGet(lua_State* L)
    {
        Plugin& plugin = self(L);
        needStorage(L, plugin);
        const std::string_view key = checkedString(L, 1);
        const auto found = std::ranges::find(plugin.storage, key, &domain::ValueMember::key);
        if (found == plugin.storage.end())
        {
            lua_pushnil(L);
        }
        else
        {
            pushValue(L, found->value);
        }
        return 1;
    }

    static int storageSet(lua_State* L)
    {
        Plugin& plugin = self(L);
        needStorage(L, plugin);
        const std::string key(checkedString(L, 1));
        if (key.empty() || key.size() > kStorageKeyMaxBytes || !isValidUtf8(key))
        {
            luaL_error(L, "storage keys are 1 to %d bytes of text", static_cast<int>(kStorageKeyMaxBytes));
        }
        domain::Value value = lua_gettop(L) >= 2 ? toValue(L, 2) : domain::Value{};
        domain::Value::Table next = plugin.storage;
        const auto found = std::ranges::find(next, key, &domain::ValueMember::key);
        if (value.isNull())
        {
            if (found != next.end())
            {
                next.erase(found);
            }
        }
        else if (found != next.end())
        {
            found->value = std::move(value);
        }
        else
        {
            next.push_back({key, std::move(value)});
        }
        const std::size_t size = json::dump(json::fromValue(domain::Value{next})).size();
        if (size > limits::kPluginStorageMaxBytes)
        {
            luaL_error(L, "storage is full (%d KB)", static_cast<int>(limits::kPluginStorageMaxBytes >> 10));
        }
        plugin.storage = std::move(next);
        plugin.storageDirty = true;
        return 0;
    }

    static int storageRemove(lua_State* L)
    {
        lua_settop(L, 1);
        return storageSet(L);
    }

    // ---- http -----------------------------------------------------------------------------------

    static int request(lua_State* L, std::string_view method)
    {
        Plugin& plugin = self(L);
        if (plugin.spec.granted.network.empty())
        {
            denied(L, "network");
        }
        const std::string url(checkedString(L, 1));
        int next = 2;
        std::string body;
        if (method == "POST")
        {
            body = std::string(checkedString(L, 2));
            if (body.size() > kHttpBodyMaxBytes)
            {
                luaL_error(L, "the request body is larger than %d KB",
                           static_cast<int>(kHttpBodyMaxBytes >> 10));
            }
            next = 3;
        }
        int options = 0;
        if (lua_type(L, next) == LUA_TTABLE)
        {
            options = next++;
        }
        luaL_checktype(L, next, LUA_TFUNCTION);

        auto parsed = parseHttpsUrl(url);
        if (!parsed)
        {
            luaL_error(L, "%s", parsed.error().message().c_str());
        }
        if (!domain::networkDomainAllowed(parsed->host, plugin.spec.granted.network))
        {
            denied(L, std::format("network access to '{}'", parsed->host));
        }
        HttpRequest out;
        out.plugin = plugin.id;
        out.method = std::string(method);
        out.host = parsed->host;
        out.target = parsed->target;
        out.body = std::move(body);
        if (options != 0)
        {
            lua_getfield(L, options, "headers");
            if (lua_type(L, -1) == LUA_TTABLE)
            {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0)
                {
                    if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING ||
                        out.headers.size() >= kMaxHeaders)
                    {
                        luaL_error(L, "headers = { [\"Name\"] = \"value\", ... } (at most %d)",
                                   static_cast<int>(kMaxHeaders));
                    }
                    std::string name(lua_tostring(L, -2));
                    std::string value(lua_tostring(L, -1));
                    if (auto checked = checkHttpHeader(name, value); !checked)
                    {
                        luaL_error(L, "%s", checked.error().message().c_str());
                    }
                    out.headers.emplace_back(std::move(name), std::move(value));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        if (plugin.httpCallbacks.size() >= kMaxInFlightHttp)
        {
            luaL_error(L, "at most %d requests at a time", static_cast<int>(kMaxInFlightHttp));
        }
        const std::uint64_t id = plugin.nextHttp++;
        plugin.httpCallbacks.emplace_back(id, lua_ref(L, next));
        const auto now = plugin.runtime->m_now;
        if (plugin.lastHttp && now - *plugin.lastHttp < kHttpInterval)
        {
            // Not an error: the answer says so, on the next turn.
            HttpResponse refused;
            refused.error = std::format("too many requests: one every {} s", kHttpInterval.count());
            plugin.lateResponses.emplace_back(id, std::move(refused));
            return 0;
        }
        plugin.lastHttp = now;
        out.id = id;
        plugin.runtime->m_host.http(std::move(out));
        return 0;
    }

    static int httpGet(lua_State* L) { return request(L, "GET"); }
    static int httpPost(lua_State* L) { return request(L, "POST"); }

    // ---- json -----------------------------------------------------------------------------------

    static int jsonEncode(lua_State* L)
    {
        luaL_checkany(L, 1);
        const std::string text = json::dump(json::fromValue(toValue(L, 1)));
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    static int jsonDecode(lua_State* L)
    {
        const std::string_view text = checkedString(L, 1);
        json::ParseLimits limits;
        limits.maxBytes = limits::kPluginNetResponseMaxBytes;
        limits.maxElements = kValueMaxElements;
        limits.maxDepth = kValueMaxDepth;
        auto parsed = json::parse(text, limits);
        if (!parsed)
        {
            lua_pushnil(L);
            lua_pushstring(L, parsed.error().message().c_str());
            return 2;
        }
        auto value = json::toValue(*parsed);
        if (!value)
        {
            lua_pushnil(L);
            lua_pushstring(L, value.error().message().c_str());
            return 2;
        }
        pushValue(L, *value);
        return 1;
    }

    // ---- the rmw table --------------------------------------------------------------------------

    static void install(Plugin& plugin)
    {
        lua_State* L = plugin.vm->L;
        lua_createtable(L, 0, 24);
        lua_pushstring(L, "1.0");
        lua_setfield(L, -2, "version");
        lua_pushlstring(L, plugin.id.data(), plugin.id.size());
        lua_setfield(L, -2, "id");
        pushValue(L,
                  plugin.spec.config.isNull() ? domain::Value{domain::Value::Table{}} : plugin.spec.config);
        lua_setfield(L, -2, "config");
        field(L, plugin, log, "log");
        field(L, plugin, warn, "warn");
        field(L, plugin, every, "every");
        field(L, plugin, after, "after");
        field(L, plugin, cancel, "cancel");
        field(L, plugin, provider, "provider");
        field(L, plugin, on, "on");
        field(L, plugin, action, "action");
        field(L, plugin, onAction, "onaction");
        field(L, plugin, widget, "widget");

        lua_createtable(L, 0, 3);
        field(L, plugin, storageGet, "get");
        field(L, plugin, storageSet, "set");
        field(L, plugin, storageRemove, "remove");
        lua_setfield(L, -2, "storage");

        lua_createtable(L, 0, 2);
        field(L, plugin, httpGet, "get");
        field(L, plugin, httpPost, "post");
        lua_setfield(L, -2, "http");

        lua_createtable(L, 0, 2);
        field(L, plugin, jsonEncode, "encode");
        field(L, plugin, jsonDecode, "decode");
        lua_setfield(L, -2, "json");
        lua_setglobal(L, "rmw");

        push(L, plugin, log, "print");
        lua_setglobal(L, "print");
        // Code from text, other environments and introspection stay out (docs/06 §6).
        for (const char* name : {"loadstring", "getfenv", "setfenv", "debug", "require", "dofile", "load"})
        {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }
    }
};

Runtime::Runtime(IPluginHost& host)
    : m_host(host)
{
}

Runtime::~Runtime() = default;

Runtime::Plugin* Runtime::find(std::string_view id) const
{
    for (const auto& plugin : m_plugins)
    {
        if (plugin->id == id)
        {
            return plugin.get();
        }
    }
    return nullptr;
}

Result<void> Runtime::load(PluginSpec spec, Clock::time_point now)
{
    m_now = now;
    const std::string id = spec.manifest.id;
    if (!spec.manifest.hasScript())
    {
        return fail(ErrorCode::InvalidArgument, std::format("{}: no script to run", id));
    }
    if (spec.source.size() > kScriptMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{}: the script is larger than {} KB", id, kScriptMaxBytes >> 10));
    }
    unload(id);

    auto plugin = std::make_unique<Plugin>();
    plugin->runtime = this;
    plugin->id = id;
    plugin->spec = std::move(spec);
    if (const auto* saved = plugin->spec.storage.table())
    {
        plugin->storage = *saved;
    }
    // Never more than the manifest asks, whatever was approved.
    plugin->spec.granted = plugin->spec.granted.intersect(plugin->spec.manifest.permissions);

    plugin->vm = std::make_unique<Vm>();
    Vm& vm = *plugin->vm;
    vm.L = lua_newstate(allocate, &vm);
    if (vm.L == nullptr)
    {
        return fail(ErrorCode::System, std::format("{}: cannot create a Luau VM", id));
    }
    lua_callbacks(vm.L)->userdata = &vm;
    lua_callbacks(vm.L)->interrupt = interrupt;

    std::size_t size = 0;
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    const std::unique_ptr<char, decltype(&std::free)> bytecode(
        luau_compile(plugin->spec.source.data(), plugin->spec.source.size(), &options, &size), &std::free);
    const std::string chunk = "=" + plugin->spec.manifest.script;
    try
    {
        luaL_openlibs(vm.L);
        Api::install(*plugin);
        luaL_sandbox(vm.L);
        vm.T = lua_newthread(vm.L);
        lua_ref(vm.L, -1); // keeps the thread alive with the VM
        lua_pop(vm.L, 1);
        luaL_sandboxthread(vm.T);
        if (bytecode == nullptr || luau_load(vm.T, chunk.c_str(), bytecode.get(), size, 0) != 0)
        {
            std::string message = lua_isstring(vm.T, -1) != 0 ? lua_tostring(vm.T, -1) : "cannot compile";
            return fail(ErrorCode::InvalidData, std::format("{}: {}", id, message));
        }
    }
    catch (...)
    {
        return fail(ErrorCode::System, std::format("{}: out of memory while starting", id));
    }

    vm.deadline = Clock::now() + kLoadBudget;
    m_host.entering(id);
    const int status = lua_pcall(vm.T, 0, 0, 0);
    m_host.left();
    if (status != LUA_OK)
    {
        std::string message = lua_isstring(vm.T, -1) != 0 ? lua_tostring(vm.T, -1) : "error";
        if (vm.capHit)
        {
            message += std::format(" (memory limit of {} MB)", limits::kPluginVmMaxBytes >> 20);
        }
        return fail(ErrorCode::InvalidData, std::format("{}: {}", id, message));
    }

    Plugin& loaded = *plugin;
    m_plugins.push_back(std::move(plugin));
    if (loaded.widgetFactory != LUA_NOREF)
    {
        for (const auto& instance : m_instances)
        {
            if (instance.plugin == id && !loaded.failed)
            {
                createInstance(loaded, instance, now);
            }
        }
    }
    else if (loaded.spec.manifest.widget)
    {
        m_host.log(id, LogLevel::Warning, "the script never calls rmw.widget: the widget keeps its defaults");
    }
    updateWatches();
    reap();
    return {};
}

void Runtime::unload(std::string_view id)
{
    const auto found = std::ranges::find_if(m_plugins, [id](const auto& plugin) { return plugin->id == id; });
    if (found == m_plugins.end())
    {
        return;
    }
    m_plugins.erase(found); // closes the VM with every reference in it
    updateWatches();
}

std::vector<PluginStatus> Runtime::status() const
{
    std::vector<PluginStatus> list;
    for (const auto& plugin : m_plugins)
    {
        list.push_back({plugin->id, plugin->failed, plugin->vm ? plugin->vm->bytes : 0,
                        static_cast<std::size_t>(std::ranges::count_if(
                            plugin->instances, [](const auto& instance) { return !instance->closed; })),
                        plugin->timers.size()});
    }
    return list;
}

bool Runtime::call(Plugin& plugin, int arguments, std::string_view what)
{
    Vm& vm = *plugin.vm;
    vm.deadline = Clock::now() + kCallbackBudget;
    vm.capHit = false;
    vm.timedOut = false;
    m_host.entering(plugin.id);
    const int status = lua_pcall(vm.T, arguments, 0, 0);
    m_host.left();
    if (status == LUA_OK)
    {
        return true;
    }
    std::string message = lua_isstring(vm.T, -1) != 0 ? lua_tostring(vm.T, -1) : "error";
    lua_pop(vm.T, 1);
    if (vm.capHit)
    {
        message += std::format(" (memory limit of {} MB)", limits::kPluginVmMaxBytes >> 20);
    }
    fault(plugin, std::format("{}: {}", what, message), m_now);
    return false;
}

// Pushes a referenced function and its arguments on the plugin's thread and calls it. Pushing can
// run out of the VM's memory outside any protected call: that ends the plugin.
bool Runtime::invokeOn(Plugin& plugin, int function, const std::function<int(lua_State*)>& arguments,
                       std::string_view what, Clock::time_point now)
{
    m_now = now;
    if (plugin.failed || plugin.vm == nullptr || function == LUA_NOREF)
    {
        return false;
    }
    lua_State* T = plugin.vm->T;
    int count = 0;
    try
    {
        lua_getref(T, function);
        count = arguments(T);
    }
    catch (...)
    {
        lua_settop(T, 0);
        plugin.failed = true;
        m_host.failed(plugin.id, std::format("{}: out of memory", what));
        return false;
    }
    return call(plugin, count, what);
}

void Runtime::fault(Plugin& plugin, std::string_view reason, Clock::time_point now)
{
    m_host.log(plugin.id, LogLevel::Error, reason);
    plugin.failures.push_back(now);
    while (!plugin.failures.empty() && now - plugin.failures.front() > kFailureWindow)
    {
        plugin.failures.pop_front();
    }
    if (static_cast<int>(plugin.failures.size()) >= kFailuresToDisable && !plugin.failed)
    {
        plugin.failed = true;
        m_host.failed(plugin.id, reason);
    }
}

void Runtime::createInstance(Plugin& plugin, const InstanceSpec& spec, Clock::time_point now)
{
    auto instance = std::make_unique<Instance>();
    instance->key = spec.key;
    instance->config = spec.config;
    Instance& created = *instance;
    lua_State* T = plugin.vm->T;
    try
    {
        Api::pushWidget(T, plugin, created);
        created.object = lua_ref(T, -1);
        lua_pop(T, 1);
    }
    catch (...)
    {
        lua_settop(T, 0);
        plugin.failed = true;
        m_host.failed(plugin.id, "out of memory while creating a widget");
        return;
    }
    plugin.instances.push_back(std::move(instance));
    invokeOn(
        plugin, plugin.widgetFactory,
        [&created](lua_State* L) {
            lua_getref(L, created.object);
            return 1;
        },
        "rmw.widget", now);
}

void Runtime::closeInstance(Plugin& plugin, Instance& instance)
{
    if (instance.closed)
    {
        return;
    }
    if (instance.onClose != LUA_NOREF)
    {
        invokeOn(plugin, instance.onClose, [](lua_State*) { return 0; }, "onclose", m_now);
    }
    instance.closed = true;
    std::erase_if(plugin.timers, [&](const Timer& timer) {
        if (timer.instance != instance.key)
        {
            return false;
        }
        plugin.unref(timer.function);
        return true;
    });
    for (const auto& [name, reference] : instance.handlers)
    {
        plugin.unref(reference);
    }
    instance.handlers.clear();
    plugin.unref(instance.onClose);
    plugin.unref(instance.object);
    instance.onClose = LUA_NOREF;
    instance.object = LUA_NOREF;
}

void Runtime::setInstances(std::span<const InstanceSpec> instances, Clock::time_point now)
{
    m_now = now;
    m_instances.assign(instances.begin(), instances.end());
    for (const auto& plugin : m_plugins)
    {
        if (plugin->failed)
        {
            continue;
        }
        for (const auto& instance : plugin->instances)
        {
            const auto wanted = std::ranges::find_if(instances, [&](const InstanceSpec& spec) {
                return spec.key == instance->key && spec.plugin == plugin->id &&
                       spec.config == instance->config;
            });
            if (wanted == instances.end())
            {
                closeInstance(*plugin, *instance);
            }
        }
        std::erase_if(plugin->instances, [](const auto& instance) { return instance->closed; });
        if (plugin->widgetFactory == LUA_NOREF)
        {
            continue;
        }
        for (const auto& spec : instances)
        {
            if (spec.plugin == plugin->id && plugin->instance(spec.key) == nullptr && !plugin->failed)
            {
                createInstance(*plugin, spec, now);
            }
        }
    }
    flush(now);
    reap();
}

void Runtime::widgetEvent(std::string_view key, std::string_view callback, const domain::Value::Array& args,
                          Clock::time_point now)
{
    m_now = now;
    for (const auto& plugin : m_plugins)
    {
        Instance* instance = plugin->instance(key);
        if (instance == nullptr || plugin->failed)
        {
            continue;
        }
        const auto handler =
            std::ranges::find(instance->handlers, callback, &std::pair<std::string, int>::first);
        if (handler == instance->handlers.end())
        {
            break;
        }
        plugin->byUser = true;
        invokeOn(
            *plugin, handler->second,
            [&args](lua_State* L) {
                for (const auto& arg : args)
                {
                    pushValue(L, arg);
                }
                return static_cast<int>(args.size());
            },
            std::format("widget callback '{}'", callback), now);
        plugin->byUser = false;
        if (Instance* still = plugin->instance(key))
        {
            still->urgent = true; // the click's answer shows at once
        }
        break;
    }
    flush(now);
    reap();
}

void Runtime::providerChanged(std::string_view name, const domain::Value& value, Clock::time_point now)
{
    m_now = now;
    for (const auto& plugin : m_plugins)
    {
        // Copied: a handler may add handlers.
        const auto handlers = plugin->providerHandlers;
        for (const auto& [provider, reference] : handlers)
        {
            if (provider != name || plugin->failed)
            {
                continue;
            }
            invokeOn(
                *plugin, reference,
                [&value](lua_State* L) {
                    pushValue(L, value);
                    return 1;
                },
                std::format("rmw.on('{}')", name), now);
        }
    }
    reap();
}

Result<void> Runtime::runAction(std::string_view id, std::string_view action, const domain::Value& args,
                                bool byUser, Clock::time_point now)
{
    m_now = now;
    Plugin* plugin = find(id);
    if (plugin == nullptr)
    {
        return fail(ErrorCode::NotFound, std::format("plugin '{}' is not running", id));
    }
    if (plugin->failed)
    {
        return fail(ErrorCode::Unsupported, std::format("plugin '{}' was stopped after repeated errors", id));
    }
    const auto handler =
        std::ranges::find(plugin->actionHandlers, action, &std::pair<std::string, int>::first);
    if (handler == plugin->actionHandlers.end())
    {
        return fail(ErrorCode::NotFound, std::format("plugin '{}' has no action '{}'", id, action));
    }
    plugin->byUser = byUser;
    const bool ok = invokeOn(
        *plugin, handler->second,
        [&args](lua_State* L) {
            pushValue(L, args);
            return 1;
        },
        std::format("action '{}'", action), now);
    plugin->byUser = false;
    flush(now);
    reap();
    if (!ok)
    {
        return fail(ErrorCode::Unknown,
                    std::format("action '{}:{}' failed (see the plugin log)", id, action));
    }
    return {};
}

void Runtime::httpDone(std::string_view id, std::uint64_t requestId, const HttpResponse& response,
                       Clock::time_point now)
{
    m_now = now;
    Plugin* plugin = find(id);
    if (plugin == nullptr)
    {
        return;
    }
    const auto found =
        std::ranges::find(plugin->httpCallbacks, requestId, &std::pair<std::uint64_t, int>::first);
    if (found == plugin->httpCallbacks.end())
    {
        return;
    }
    const int reference = found->second;
    plugin->httpCallbacks.erase(found);
    invokeOn(
        *plugin, reference,
        [&response](lua_State* L) {
            lua_createtable(L, 0, 5);
            lua_pushnumber(L, response.status);
            lua_setfield(L, -2, "status");
            lua_pushboolean(L, response.status >= 200 && response.status < 300 ? 1 : 0);
            lua_setfield(L, -2, "ok");
            lua_pushlstring(L, response.body.data(), response.body.size());
            lua_setfield(L, -2, "body");
            lua_pushlstring(L, response.contentType.data(), response.contentType.size());
            lua_setfield(L, -2, "type");
            if (!response.error.empty())
            {
                lua_pushlstring(L, response.error.data(), response.error.size());
                lua_setfield(L, -2, "error");
            }
            return 1;
        },
        "http callback", now);
    plugin->unref(reference);
    flush(now);
    reap();
}

void Runtime::tick(Clock::time_point now)
{
    m_now = now;
    for (const auto& plugin : m_plugins)
    {
        auto late = std::move(plugin->lateResponses);
        plugin->lateResponses.clear();
        for (const auto& [id, response] : late)
        {
            httpDone(plugin->id, id, response, now);
        }
        std::vector<std::uint64_t> due;
        for (const auto& timer : plugin->timers)
        {
            if (timer.due <= now)
            {
                due.push_back(timer.id);
            }
        }
        for (const std::uint64_t id : due)
        {
            if (plugin->failed)
            {
                break;
            }
            const auto timer = std::ranges::find(plugin->timers, id, &Timer::id);
            if (timer == plugin->timers.end())
            {
                continue; // cancelled by an earlier one
            }
            const int function = timer->function;
            const bool once = timer->period.count() == 0;
            if (once)
            {
                plugin->timers.erase(timer);
            }
            else
            {
                // Late turns are skipped, not caught up.
                while (timer->due <= now)
                {
                    timer->due += timer->period;
                }
            }
            invokeOn(*plugin, function, [](lua_State*) { return 0; }, "timer", now);
            if (once)
            {
                plugin->unref(function);
            }
        }
    }
    flush(now);
    reap();
}

void Runtime::flush(Clock::time_point now)
{
    for (const auto& plugin : m_plugins)
    {
        if (plugin->failed)
        {
            continue;
        }
        for (const auto& instance : plugin->instances)
        {
            if (instance->closed || instance->pending.empty())
            {
                continue;
            }
            if (instance->urgent || now - instance->lastFlush >= kWidgetUpdateInterval)
            {
                m_host.setWidget(instance->key, domain::Value{std::move(instance->pending)});
                instance->pending.clear();
                instance->lastFlush = now;
                instance->urgent = false;
            }
        }
    }
}

std::optional<Clock::time_point> Runtime::nextWake() const
{
    std::optional<Clock::time_point> next;
    const auto consider = [&next](Clock::time_point at) {
        if (!next || at < *next)
        {
            next = at;
        }
    };
    for (const auto& plugin : m_plugins)
    {
        if (plugin->failed)
        {
            continue;
        }
        for (const auto& timer : plugin->timers)
        {
            consider(timer.due);
        }
        if (!plugin->lateResponses.empty())
        {
            consider(m_now);
        }
        for (const auto& instance : plugin->instances)
        {
            if (!instance->closed && !instance->pending.empty())
            {
                consider(instance->urgent ? m_now : instance->lastFlush + kWidgetUpdateInterval);
            }
        }
    }
    return next;
}

std::vector<std::pair<std::string, domain::Value>> Runtime::takeDirtyStorage()
{
    std::vector<std::pair<std::string, domain::Value>> dirty;
    for (const auto& plugin : m_plugins)
    {
        if (plugin->storageDirty)
        {
            dirty.emplace_back(plugin->id, domain::Value{plugin->storage});
            plugin->storageDirty = false;
        }
    }
    return dirty;
}

void Runtime::updateWatches()
{
    std::vector<std::string> names;
    for (const auto& plugin : m_plugins)
    {
        if (plugin->failed)
        {
            continue;
        }
        for (const auto& [name, reference] : plugin->providerHandlers)
        {
            if (!contains(names, name))
            {
                names.push_back(name);
            }
        }
    }
    std::ranges::sort(names);
    if (names != m_watched)
    {
        m_watched = std::move(names);
        m_host.watchProviders(m_watched);
    }
}

void Runtime::reap()
{
    bool changed = false;
    for (const auto& plugin : m_plugins)
    {
        if (plugin->failed && plugin->vm != nullptr)
        {
            // Stopped: drop its VM (and the memory it holds); it stays listed as failed.
            plugin->timers.clear();
            plugin->instances.clear();
            plugin->providerHandlers.clear();
            plugin->actionHandlers.clear();
            plugin->httpCallbacks.clear();
            plugin->lateResponses.clear();
            plugin->widgetFactory = LUA_NOREF;
            plugin->vm.reset();
            changed = true;
        }
    }
    if (changed)
    {
        updateWatches();
    }
}

} // namespace rmw::plugins
