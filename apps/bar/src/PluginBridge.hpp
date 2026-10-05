#pragma once

#include "rmw/bar/Bridge.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace rmw::barapp
{

// The bar's end of the plugin host's pipe (docs/12 §3): the core names it, the bar serves it, and
// only rmw-plugin-host.exe from our own folder may connect. Messages arrive on the UI thread
// (through Slint's event loop); sending never blocks it.
class PluginBridge
{
public:
    using Handler = std::function<void(bar::bridge::Message)>;

    // `connected` runs on the UI thread each time a host connects (send it the widgets then).
    PluginBridge(std::wstring pipe, std::filesystem::path hostImage, Handler handle,
                 std::function<void()> connected);
    ~PluginBridge();
    PluginBridge(const PluginBridge&) = delete;
    PluginBridge& operator=(const PluginBridge&) = delete;
    PluginBridge(PluginBridge&&) = delete;
    PluginBridge& operator=(PluginBridge&&) = delete;

    void send(const bar::bridge::Message& message);

    struct State;

private:
    std::shared_ptr<State> m_state;
    std::jthread m_accept;
};

} // namespace rmw::barapp
