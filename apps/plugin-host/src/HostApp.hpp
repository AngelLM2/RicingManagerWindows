#pragma once

#include "Links.hpp"
#include "ProviderHub.hpp"

#include "rmw/bar/Bridge.hpp"
#include "rmw/platform/Channel.hpp"
#include "rmw/platform/Supervisor.hpp"
#include "rmw/plugins/HostPlan.hpp"
#include "rmw/scripting/Runtime.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rmw::pluginhost
{

struct HostOptions
{
    std::filesystem::path dataDirectory;
    std::filesystem::path planFile; // <data>\plugin-host\plan.json
    std::wstring bridgePipe;        // served by rmw-bar; empty: no bar
    std::wstring controlPipe;       // served by us, for the core
    void* blame = nullptr;          // inherited mapping: which plugin ran when we died
};

using plugins::BlameRecord;

// rmw-plugin-host's single thread of work: plugins run here, one callback at a time, between
// messages from the bar, the core, the providers and the network. It beats the supervisor's
// heartbeat every turn, so a plugin stuck inside a C function ends the process (and only it).
class HostApp final : public scripting::IPluginHost
{
public:
    HostApp(HostOptions options, platform::Heartbeat& heartbeat);
    ~HostApp() override;
    HostApp(const HostApp&) = delete;
    HostApp& operator=(const HostApp&) = delete;
    HostApp(HostApp&&) = delete;
    HostApp& operator=(HostApp&&) = delete;

    [[nodiscard]] int run();

    // ---- scripting::IPluginHost (main thread) ----
    domain::Value provider(std::string_view name) override;
    void watchProviders(std::span<const std::string> names) override;
    void action(const std::string& plugin, const std::string& name, const domain::Value& args,
                bool byUser) override;
    void setWidget(const std::string& instance, const domain::Value& properties) override;
    void http(scripting::HttpRequest request) override;
    void log(const std::string& plugin, scripting::LogLevel level, std::string_view text) override;
    void entering(const std::string& plugin) override;
    void left() override;
    void failed(const std::string& plugin, std::string_view reason) override;

private:
    struct Watch;

    void reloadPlan();
    void load(const plugins::HostPlugin& plugin);
    void connectBridge(Clock::time_point now);
    void onBridge(const std::string& text);
    void bridgeClosed();
    void send(const bar::bridge::Message& message);
    void saveStorage();
    void watchDevFolders();
    [[nodiscard]] Result<json::Json> control(const ipc::Request& request);
    [[nodiscard]] const plugins::HostPlugin* planned(std::string_view id) const;

    HostOptions m_options;
    platform::Heartbeat& m_heartbeat;
    std::shared_ptr<Inbox> m_inbox = std::make_shared<Inbox>();
    BlameRecord* m_blame = nullptr;
    ProviderHub m_providers;
    scripting::Runtime m_runtime{*this};
    plugins::HostPlan m_plan;
    std::map<std::string, std::string, std::less<>> m_loadErrors;
    std::vector<bar::bridge::InstanceInfo> m_instances;
    std::optional<Clock::time_point> m_planDue;
    std::map<std::string, Clock::time_point, std::less<>> m_devDue;
    std::vector<std::unique_ptr<Watch>> m_watches; // plan folder first, then dev plugin folders
    Clock::time_point m_nextBridgeTry{};
    Clock::time_point m_nextSave{};
    std::atomic<int> m_requests{0}; // HTTP requests in flight
    bool m_stop = false;
    std::filesystem::path m_folder; // our executable's folder (where rmw-bar and rmw-core live)
    std::unique_ptr<CoreLink> m_core;
    std::unique_ptr<platform::MessageChannel> m_bridge; // after the inbox: its threads post into it
    std::unique_ptr<ControlServer> m_control;
};

} // namespace rmw::pluginhost
