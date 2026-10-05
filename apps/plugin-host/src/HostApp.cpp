#include "HostApp.hpp"

#include "Http.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/json/Value.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <thread>

namespace rmw::pluginhost
{
namespace
{

using namespace std::chrono_literals;
using json::Json;

constexpr auto kPlanSettle = 100ms;
constexpr auto kDevSettle = 150ms;
constexpr auto kBridgeRetry = 1s;
constexpr auto kSaveEvery = 5s;
constexpr auto kHeartbeatEvery = 250ms;
constexpr std::uint64_t kManifestMaxBytes = limits::kManifestMaxBytes;
constexpr std::uint64_t kPlanMaxBytes = 1u << 20;
constexpr int kMaxRequests = 8;
constexpr std::size_t kMaxDevWatches = 32;

// Actions only the bar can do: it got the click, so Windows lets it bring a window or an app forward.
constexpr auto kBarActions = std::to_array<std::string_view>(
    {"window.activate", "window.focus", "window.minimize", "window.close", "tray.click"});
constexpr auto kMediaCommands =
    std::to_array<std::string_view>({"toggle", "play", "pause", "next", "previous"});

Result<std::string> readText(const std::filesystem::path& file, std::uint64_t maxBytes)
{
    auto bytes = platform::readFileLimited(file, maxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

std::filesystem::path storageFile(const std::filesystem::path& data, std::string_view id)
{
    return data / L"plugin-data" / (utf8To16Lossy(id) + L".json");
}

} // namespace

// A folder whose changes we wait for (the plan's, or a plugin being written).
struct HostApp::Watch
{
    std::string plugin; // empty: the plan's folder
    HANDLE handle = INVALID_HANDLE_VALUE;

    Watch(std::string id, const std::filesystem::path& folder, bool subtree)
        : plugin(std::move(id))
        , handle(FindFirstChangeNotificationW(folder.c_str(), subtree ? TRUE : FALSE,
                                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                  FILE_NOTIFY_CHANGE_DIR_NAME))
    {
    }
    ~Watch()
    {
        if (handle != INVALID_HANDLE_VALUE)
        {
            FindCloseChangeNotification(handle);
        }
    }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&&) = delete;
    Watch& operator=(Watch&&) = delete;
};

HostApp::HostApp(HostOptions options, platform::Heartbeat& heartbeat)
    : m_options(std::move(options))
    , m_heartbeat(heartbeat)
    , m_providers([inbox = m_inbox](std::function<void()> task) { inbox->post(std::move(task)); },
                  [this](std::string_view name, const domain::Value& value) {
                      m_runtime.providerChanged(name, value, Clock::now());
                  })
{
    if (m_options.blame != nullptr)
    {
        m_blame = static_cast<BlameRecord*>(
            MapViewOfFile(m_options.blame, FILE_MAP_WRITE, 0, 0, sizeof(BlameRecord)));
    }
    if (auto self = platform::currentExecutable())
    {
        m_folder = self->parent_path();
    }
    m_core = std::make_unique<CoreLink>(m_folder / L"rmw-core.exe");
    if (!m_options.controlPipe.empty())
    {
        m_control =
            std::make_unique<ControlServer>(m_options.controlPipe, m_folder / L"rmw-core.exe", m_inbox,
                                            [this](const ipc::Request& request) { return control(request); });
    }
}

HostApp::~HostApp()
{
    m_control.reset();
    m_bridge.reset();
    saveStorage();
    if (m_blame != nullptr)
    {
        UnmapViewOfFile(m_blame);
    }
}

int HostApp::run()
{
    std::filesystem::create_directories(m_options.planFile.parent_path());
    std::filesystem::create_directories(m_options.dataDirectory / L"plugin-data");
    reloadPlan();
    while (!m_stop)
    {
        for (auto& task : m_inbox->take())
        {
            task();
        }
        const auto now = Clock::now();
        if (m_planDue && *m_planDue <= now)
        {
            m_planDue.reset();
            reloadPlan();
        }
        for (auto it = m_devDue.begin(); it != m_devDue.end();)
        {
            if (it->second > now)
            {
                ++it;
                continue;
            }
            if (const auto* plugin = planned(it->first))
            {
                rmw::log::info("plugin host: {} changed on disk: reloading", it->first);
                const auto copy = *plugin;
                load(copy);
            }
            it = m_devDue.erase(it);
        }
        connectBridge(now);
        Clock::time_point next = m_providers.poll(now);
        m_runtime.tick(Clock::now());
        if (now >= m_nextSave)
        {
            saveStorage();
            m_nextSave = now + kSaveEvery;
        }
        m_heartbeat.beat();

        next = std::min(
            {next, now + kHeartbeatEvery, m_nextBridgeTry > now ? m_nextBridgeTry : now + kBridgeRetry});
        if (const auto wake = m_runtime.nextWake())
        {
            next = std::min(next, *wake);
        }
        if (m_planDue)
        {
            next = std::min(next, *m_planDue);
        }
        for (const auto& [id, due] : m_devDue)
        {
            next = std::min(next, due);
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now());
        std::vector<HANDLE> handles{m_inbox->event()};
        for (const auto& watch : m_watches)
        {
            if (watch->handle != INVALID_HANDLE_VALUE)
            {
                handles.push_back(watch->handle);
            }
        }
        const DWORD signaled =
            MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(),
                                        static_cast<DWORD>(std::clamp<std::int64_t>(wait.count(), 0, 1000)),
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (signaled > WAIT_OBJECT_0 && signaled < WAIT_OBJECT_0 + handles.size())
        {
            void* const handle = handles[signaled - WAIT_OBJECT_0];
            for (const auto& watch : m_watches)
            {
                if (watch->handle != handle)
                {
                    continue;
                }
                FindNextChangeNotification(handle);
                if (watch->plugin.empty())
                {
                    m_planDue = Clock::now() + kPlanSettle;
                }
                else
                {
                    m_devDue[watch->plugin] = Clock::now() + kDevSettle;
                }
            }
        }
        // Window events (the WinEvent hooks) arrive as messages to this thread.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                m_stop = true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}

// ---- the plan -----------------------------------------------------------------------------------

const plugins::HostPlugin* HostApp::planned(std::string_view id) const
{
    const auto found = std::ranges::find(m_plan.plugins, id, &plugins::HostPlugin::id);
    return found == m_plan.plugins.end() ? nullptr : &*found;
}

void HostApp::reloadPlan()
{
    auto text = readText(m_options.planFile, kPlanMaxBytes);
    plugins::HostPlan next;
    if (text)
    {
        auto parsed = plugins::hostPlanFromJson(*text);
        if (!parsed)
        {
            rmw::log::error("plugin host: the plan is invalid: {}", parsed.error().toString());
            return; // keep running what runs
        }
        next = std::move(*parsed);
    }
    for (const auto& old : m_plan.plugins)
    {
        if (std::ranges::find(next.plugins, old.id, &plugins::HostPlugin::id) == next.plugins.end())
        {
            rmw::log::info("plugin host: {} stopped", old.id);
            m_runtime.unload(old.id);
            m_loadErrors.erase(old.id);
        }
    }
    const auto previous = std::exchange(m_plan, std::move(next));
    for (const auto& plugin : m_plan.plugins)
    {
        const auto before = std::ranges::find(previous.plugins, plugin.id, &plugins::HostPlugin::id);
        if (before == previous.plugins.end() || !(*before == plugin))
        {
            load(plugin);
        }
    }
    watchDevFolders();
}

void HostApp::load(const plugins::HostPlugin& plugin)
{
    const auto failed = [&](const std::string& reason) {
        rmw::log::error("plugin host: {} not started: {}", plugin.id, reason);
        m_loadErrors[plugin.id] = reason;
        m_runtime.unload(plugin.id);
    };
    auto manifestText = readText(plugin.folder / L"plugin.toml", kManifestMaxBytes);
    if (!manifestText)
    {
        failed(manifestText.error().message());
        return;
    }
    domain::Diagnostics diagnostics;
    auto manifest = domain::parsePluginManifest(*manifestText, diagnostics, {});
    if (!manifest)
    {
        failed(manifest.error().message());
        return;
    }
    if (manifest->id != plugin.id)
    {
        failed(std::format("the folder holds plugin '{}'", manifest->id));
        return;
    }
    if (!manifest->hasScript())
    {
        m_runtime.unload(plugin.id); // a static widget, a skin or tweaks: nothing to run
        m_loadErrors.erase(plugin.id);
        return;
    }
    auto source = readText(plugin.folder / std::filesystem::path(utf8To16Lossy(manifest->script)),
                           scripting::kScriptMaxBytes);
    if (!source)
    {
        failed(source.error().message());
        return;
    }
    scripting::PluginSpec spec;
    spec.manifest = std::move(*manifest);
    spec.granted = plugin.granted;
    spec.config = plugin.config;
    spec.source = std::move(*source);
    if (spec.granted.storage)
    {
        if (auto saved =
                readText(storageFile(m_options.dataDirectory, plugin.id), limits::kPluginStorageMaxBytes * 2))
        {
            if (auto parsed = json::parse(*saved); parsed)
            {
                if (auto value = json::toValue(*parsed); value && value->table() != nullptr)
                {
                    spec.storage = std::move(*value);
                }
            }
        }
    }
    if (auto loaded = m_runtime.load(std::move(spec), Clock::now()); !loaded)
    {
        failed(loaded.error().message());
        return;
    }
    m_loadErrors.erase(plugin.id);
    rmw::log::info("plugin host: {} running", plugin.id);
}

void HostApp::watchDevFolders()
{
    m_watches.clear();
    m_watches.push_back(std::make_unique<Watch>(std::string(), m_options.planFile.parent_path(), false));
    for (const auto& plugin : m_plan.plugins)
    {
        if (plugin.dev && m_watches.size() <= kMaxDevWatches)
        {
            m_watches.push_back(std::make_unique<Watch>(plugin.id, plugin.folder, true));
        }
    }
}

void HostApp::saveStorage()
{
    for (const auto& [id, storage] : m_runtime.takeDirtyStorage())
    {
        const std::string text = json::dump(json::fromValue(storage));
        if (auto saved = platform::writeFileAtomic(storageFile(m_options.dataDirectory, id),
                                                   std::as_bytes(std::span(text)));
            !saved)
        {
            rmw::log::warn("plugin host: cannot save the storage of {}: {}", id, saved.error().toString());
        }
    }
}

// ---- the bar ------------------------------------------------------------------------------------

void HostApp::connectBridge(Clock::time_point now)
{
    if (m_bridge || m_options.bridgePipe.empty() || now < m_nextBridgeTry)
    {
        return;
    }
    m_nextBridgeTry = now + kBridgeRetry;
    auto connection = platform::connectPipe(m_options.bridgePipe, 200ms, m_folder / L"rmw-bar.exe");
    if (!connection)
    {
        return; // no bar yet (or none at all): try again in a moment
    }
    const auto inbox = m_inbox;
    m_bridge = std::make_unique<platform::MessageChannel>(
        std::move(*connection),
        [inbox, this](std::string message) {
            inbox->post([this, message = std::move(message)] { onBridge(message); });
        },
        [inbox, this] { inbox->post([this] { bridgeClosed(); }); });
    send(bar::bridge::Hello{});
    if (!m_barWatched.empty())
    {
        send(bar::bridge::Watch{m_barWatched});
    }
    rmw::log::info("plugin host: connected to the bar");
}

void HostApp::bridgeClosed()
{
    if (!m_bridge)
    {
        return;
    }
    rmw::log::info("plugin host: the bar went away");
    m_bridge.reset();
    m_instances.clear();
    m_runtime.setInstances({}, Clock::now());
}

void HostApp::send(const bar::bridge::Message& message)
{
    if (m_bridge && !m_bridge->send(bar::bridge::encode(message)))
    {
        rmw::log::warn("plugin host: the bar is not reading; an update was dropped");
    }
}

void HostApp::onBridge(const std::string& text)
{
    auto message = bar::bridge::decode(text);
    if (!message)
    {
        rmw::log::warn("plugin host: bad message from the bar: {}", message.error().message());
        return;
    }
    const auto now = Clock::now();
    if (const auto* hello = std::get_if<bar::bridge::Hello>(&*message))
    {
        if (hello->version != bar::bridge::kVersion)
        {
            rmw::log::error("plugin host: the bar speaks bridge version {} (we speak {})", hello->version,
                            bar::bridge::kVersion);
            m_bridge.reset();
        }
    }
    else if (auto* instances = std::get_if<bar::bridge::Instances>(&*message))
    {
        m_instances = std::move(instances->list);
        std::vector<scripting::InstanceSpec> specs;
        specs.reserve(m_instances.size());
        for (const auto& instance : m_instances)
        {
            specs.push_back({instance.key, instance.plugin, instance.config});
        }
        m_runtime.setInstances(specs, now);
    }
    else if (const auto* event = std::get_if<bar::bridge::Event>(&*message))
    {
        m_runtime.widgetEvent(event->key, event->callback, event->arguments, now);
    }
    else if (auto* provided = std::get_if<bar::bridge::Provide>(&*message))
    {
        if (std::ranges::find(m_barWatched, provided->name) != m_barWatched.end())
        {
            m_providers.fromBar(provided->name, std::move(provided->value));
        }
    }
}

// ---- scripting::IPluginHost -----------------------------------------------------------------------

domain::Value HostApp::provider(std::string_view name)
{
    return m_providers.value(name);
}

void HostApp::watchProviders(std::span<const std::string> names)
{
    m_providers.watch(names);
    // The tray is the bar's: it sends what it sees while some plugin watches it.
    std::vector<std::string> fromBar;
    for (const auto& name : names)
    {
        if (bar::bridge::isBarProvider(name))
        {
            fromBar.push_back(name);
        }
    }
    if (fromBar != m_barWatched)
    {
        m_barWatched = std::move(fromBar);
        send(bar::bridge::Watch{m_barWatched});
    }
}

void HostApp::action(const std::string& plugin, const std::string& name, const domain::Value& args,
                     bool byUser)
{
    if (std::ranges::find(kBarActions, name) != kBarActions.end())
    {
        if (!m_bridge)
        {
            rmw::log::warn("plugin host: {}: {} needs the bar running", plugin, name);
            return;
        }
        send(bar::bridge::Action{name, args});
        return;
    }
    if (name == "media.control")
    {
        const auto* command = args.find("command");
        const auto* text = command != nullptr ? std::get_if<std::string>(&command->data) : nullptr;
        if (text == nullptr || std::ranges::find(kMediaCommands, *text) == kMediaCommands.end())
        {
            rmw::log::warn(
                "plugin host: {}: media.control takes {{ command = \"toggle\" | \"play\" | \"pause\" | "
                "\"next\" | \"previous\" }}",
                plugin);
            return;
        }
        m_providers.media(*text);
        return;
    }
    Json params = Json::object();
    if (name == "rice.apply")
    {
        // A rice that comes with the plugin, by its folder inside the plugin's.
        const auto* rice = args.find("rice");
        const auto* text = rice != nullptr ? std::get_if<std::string>(&rice->data) : nullptr;
        const auto* entry = planned(plugin);
        auto relative = text != nullptr ? domain::validatePackagePath(*text)
                                        : Result<std::string>(fail(ErrorCode::InvalidArgument, "missing"));
        if (entry == nullptr || !relative)
        {
            rmw::log::warn("plugin host: {}: rice.apply takes {{ rice = \"<folder in the plugin>\" }}",
                           plugin);
            return;
        }
        const auto folder = entry->folder / std::filesystem::path(utf8To16Lossy(*relative));
        const auto final = platform::finalPath(folder);
        const auto root = platform::finalPath(entry->folder);
        if (!final || !root || !platform::isWithin(*final, *root) ||
            !std::filesystem::is_regular_file(*final / L"rice.toml"))
        {
            rmw::log::warn("plugin host: {}: '{}' is not a rice in the plugin's folder", plugin, *relative);
            return;
        }
        params["path"] = utf16To8Lossy(final->native());
    }
    rmw::log::info("plugin host: {} asks for {}", plugin, name);
    m_core->call("action.run",
                 Json{{"name", name}, {"params", std::move(params)}, {"plugin", plugin}, {"byUser", byUser}});
}

void HostApp::setWidget(const std::string& instance, const domain::Value& properties)
{
    send(bar::bridge::Set{instance, properties});
}

void HostApp::http(scripting::HttpRequest request)
{
    if (m_requests >= kMaxRequests)
    {
        scripting::HttpResponse busy;
        busy.error = "the host has too many requests in flight";
        m_inbox->post([this, plugin = request.plugin, id = request.id, busy] {
            m_runtime.httpDone(plugin, id, busy, Clock::now());
        });
        return;
    }
    ++m_requests;
    std::thread([inbox = m_inbox, this, request = std::move(request)] {
        auto response = fetch(request);
        inbox->post([this, plugin = request.plugin, id = request.id, response = std::move(response)] {
            --m_requests;
            m_runtime.httpDone(plugin, id, response, Clock::now());
        });
    }).detach();
}

void HostApp::log(const std::string& plugin, scripting::LogLevel level, std::string_view text)
{
    switch (level)
    {
    case scripting::LogLevel::Info:
        rmw::log::info("[{}] {}", plugin, text);
        break;
    case scripting::LogLevel::Warning:
        rmw::log::warn("[{}] {}", plugin, text);
        break;
    case scripting::LogLevel::Error:
        rmw::log::error("[{}] {}", plugin, text);
        break;
    }
}

void HostApp::entering(const std::string& plugin)
{
    if (m_blame != nullptr)
    {
        const std::size_t length = std::min(plugin.size(), m_blame->plugin.size() - 1);
        std::memcpy(m_blame->plugin.data(), plugin.data(), length);
        m_blame->plugin[length] = '\0';
    }
}

void HostApp::left()
{
    if (m_blame != nullptr)
    {
        m_blame->plugin[0] = '\0';
    }
}

void HostApp::failed(const std::string& plugin, std::string_view reason)
{
    rmw::log::error("plugin host: {} was stopped after repeated failures: {}", plugin, reason);
    m_loadErrors[plugin] = std::format("stopped after repeated failures: {}", reason);
    m_core->call("plugin.report", Json{{"plugin", plugin}, {"reason", reason}});
}

// ---- control ------------------------------------------------------------------------------------

Result<Json> HostApp::control(const ipc::Request& request)
{
    if (request.method == "status")
    {
        Json plugins = Json::array();
        for (const auto& status : m_runtime.status())
        {
            plugins.push_back({{"id", status.id},
                               {"failed", status.failed},
                               {"memory", status.memoryBytes},
                               {"widgets", status.instances},
                               {"timers", status.timers}});
        }
        Json errors = Json::object();
        for (const auto& [id, reason] : m_loadErrors)
        {
            errors[id] = reason;
        }
        return Json{
            {"plugins", std::move(plugins)}, {"errors", std::move(errors)}, {"bar", m_bridge != nullptr}};
    }
    if (request.method == "plugin.action")
    {
        const std::string plugin = json::stringOr(request.params, "plugin", "");
        const std::string action = json::stringOr(request.params, "action", "");
        auto args = json::toValue(json::memberOr(request.params, "args", Json::object()));
        if (!args)
        {
            return std::unexpected(std::move(args.error()));
        }
        auto ran = m_runtime.runAction(plugin, action, *args, json::boolOr(request.params, "byUser", true),
                                       Clock::now());
        if (!ran)
        {
            return std::unexpected(std::move(ran.error()));
        }
        return Json{{"ok", true}};
    }
    if (request.method == "reload")
    {
        reloadPlan();
        return Json{{"ok", true}};
    }
    if (request.method == "stop")
    {
        saveStorage();
        m_stop = true;
        return Json{{"ok", true}};
    }
    return fail(ErrorCode::NotFound, std::format("unknown method '{}'", request.method));
}

} // namespace rmw::pluginhost
