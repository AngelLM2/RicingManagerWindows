#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/base/Limits.hpp"
#include "rmw/domain/PluginManifest.hpp"
#include "rmw/domain/Value.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

// The Luau side of rmw-plugin-host (docs/12 §3, P2): one sandboxed VM per plugin and the rmw.*
// API its script sees. Everything a script asks for is checked here against what the user
// approved; everything it gets from outside comes through IPluginHost, so this runs and is tested
// without Windows.
//
// Three locks, because none is enough alone (docs/06 §6): an interrupt ends any callback that runs
// longer than limits::kPluginCallbackMaxMs; an allocator caps each VM at limits::kPluginVmMaxBytes;
// and the process watchdog (the supervisor's heartbeat) ends the host when a C function runs long
// (a pathological string pattern), with entering()/left() saying whom to blame.
namespace rmw::scripting
{

using Clock = std::chrono::steady_clock;

inline constexpr std::chrono::milliseconds kMinRepeatInterval{limits::kPluginMinUpdateIntervalMs};
inline constexpr std::chrono::milliseconds kMinDelay{50};
inline constexpr std::chrono::milliseconds kWidgetUpdateInterval{limits::kPluginMinUpdateIntervalMs};
inline constexpr std::chrono::seconds kHttpInterval{10};
inline constexpr int kFailuresToDisable = 3;
inline constexpr std::chrono::minutes kFailureWindow{5};
inline constexpr std::size_t kScriptMaxBytes = 256u << 10;

struct HttpRequest
{
    std::string plugin;
    std::uint64_t id = 0;
    std::string method; // "GET" or "POST"
    std::string host;   // checked against the plugin's domains
    std::string target; // "/path?query"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse
{
    int status = 0; // 0: no answer (see `error`)
    std::string body;
    std::string contentType;
    std::string error;
};

enum class LogLevel
{
    Info,
    Warning,
    Error,
};

// What the runtime needs from the process around it. Calls never re-enter the runtime.
class IPluginHost
{
public:
    virtual ~IPluginHost() = default;

    // The latest data of a provider (null before the first sample). Permission is already checked.
    virtual domain::Value provider(std::string_view name) = 0;
    // The providers some plugin listens to now: start or stop sampling them.
    virtual void watchProviders(std::span<const std::string> names) = 0;
    // A permitted action. `byUser`: in answer to a click or a hotkey (not a timer).
    virtual void action(const std::string& plugin, const std::string& name, const domain::Value& args,
                        bool byUser) = 0;
    // New values for some properties of a widget instance (already checked against the manifest).
    virtual void setWidget(const std::string& instance, const domain::Value& properties) = 0;
    virtual void http(HttpRequest request) = 0;
    virtual void log(const std::string& plugin, LogLevel level, std::string_view text) = 0;
    // A callback of `plugin` runs from now until left().
    virtual void entering(const std::string& plugin) = 0;
    virtual void left() = 0;
    // The plugin failed kFailuresToDisable times in kFailureWindow: it is stopped until reloaded.
    virtual void failed(const std::string& plugin, std::string_view reason) = 0;
};

struct PluginSpec
{
    domain::PluginManifest manifest;
    domain::PluginPermissions granted; // what the user approved (never more than the manifest asks)
    domain::Value config;              // resolved settings (a table)
    std::string source;                // the script's text
    domain::Value storage;             // what rmw.storage held last time (a table, or null)
};

struct InstanceSpec
{
    std::string key;    // unique per widget on a bar: "<bar>/<slot>"
    std::string plugin; // its plugin
    domain::Value config;
};

struct PluginStatus
{
    std::string id;
    bool failed = false;
    std::size_t memoryBytes = 0;
    std::size_t instances = 0;
    std::size_t timers = 0;
};

class Runtime
{
public:
    explicit Runtime(IPluginHost& host);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    // Starts a plugin in a fresh VM (replacing one with the same id): its script runs once and
    // registers what it does. Widgets of the plugin that were shown are created again. What the one
    // it replaces stored and was not taken yet (takeDirtyStorage) wins over `spec.storage`.
    [[nodiscard]] Result<void> load(PluginSpec spec, Clock::time_point now);
    // Its storage, when not taken yet, is still given by takeDirtyStorage().
    void unload(std::string_view id);
    [[nodiscard]] std::vector<PluginStatus> status() const;

    // The plugin widgets on the bars now: new ones are created, missing ones closed.
    void setInstances(std::span<const InstanceSpec> instances, Clock::time_point now);
    // A widget callback fired (a click). Its updates go out at once.
    void widgetEvent(std::string_view instance, std::string_view callback, const domain::Value::Array& args,
                     Clock::time_point now);
    void providerChanged(std::string_view name, const domain::Value& value, Clock::time_point now);
    // "<plugin>:<action>" from a hotkey, a rule or the CLI.
    [[nodiscard]] Result<void> runAction(std::string_view plugin, std::string_view action,
                                         const domain::Value& args, bool byUser, Clock::time_point now);
    void httpDone(std::string_view plugin, std::uint64_t id, const HttpResponse& response,
                  Clock::time_point now);

    // Runs the timers due by `now`, then sends the widget updates that may go.
    void tick(Clock::time_point now);
    // When tick() has something to do (nullopt: nothing scheduled).
    [[nodiscard]] std::optional<Clock::time_point> nextWake() const;
    // rmw.storage tables changed since the last call (to be saved).
    [[nodiscard]] std::vector<std::pair<std::string, domain::Value>> takeDirtyStorage();

    struct Plugin;
    struct Instance;

private:
    friend struct Api;

    Plugin* find(std::string_view id) const;
    bool call(Plugin& plugin, int arguments, std::string_view what);
    bool invokeOn(Plugin& plugin, int function, const std::function<int(lua_State*)>& arguments,
                  std::string_view what, Clock::time_point now);
    void flush(Clock::time_point now);
    void fault(Plugin& plugin, std::string_view reason, Clock::time_point now);
    void createInstance(Plugin& plugin, const InstanceSpec& spec, Clock::time_point now);
    void closeInstance(Plugin& plugin, Instance& instance);
    void updateWatches();
    void reap();

    IPluginHost& m_host;
    std::vector<std::unique_ptr<Plugin>> m_plugins;
    std::vector<InstanceSpec> m_instances; // what the bars show (also for plugins loaded later)
    std::vector<std::string> m_watched;
    // rmw.storage of plugins unloaded before it was taken: v1.7 lost up to 5 s of it on each reload.
    std::vector<std::pair<std::string, domain::Value::Table>> m_unsavedStorage;
    Clock::time_point m_now;
};

// Compiles a script without running it (rmw plugin validate): its syntax errors, with lines.
[[nodiscard]] Result<void> checkScript(std::string_view source, std::string_view name);

} // namespace rmw::scripting
