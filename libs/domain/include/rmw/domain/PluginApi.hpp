#pragma once

#include "rmw/domain/SemVer.hpp"

#include <span>
#include <string>
#include <string_view>

// What a plugin may ask for (docs/12 §5): the providers it reads, the actions it calls. One list,
// shared by the manifest parser, the install prompt, the plugin host (which checks every call)
// and the docs (rmw.d.luau).
namespace rmw::domain
{

// The plugin API this build implements (docs/12 §7). A manifest's `api = "^1.0"` must match it.
// Marked as preview until Ricing Manager 1.0.
[[nodiscard]] SemVer pluginApiVersion() noexcept;

struct PluginCapability
{
    std::string_view name;
    // Shown highlighted at install time: privacy (window titles, what is playing) or reach
    // (closing windows).
    bool sensitive = false;
    std::string_view summary; // for the install prompt: "read the CPU usage"
};

[[nodiscard]] std::span<const PluginCapability> pluginProviders() noexcept;
[[nodiscard]] std::span<const PluginCapability> pluginActions() noexcept;
[[nodiscard]] const PluginCapability* findPluginProvider(std::string_view name) noexcept;
[[nodiscard]] const PluginCapability* findPluginAction(std::string_view name) noexcept;

// A domain a plugin may reach through the host (docs/12 §6): a lower-case DNS name with at least
// two labels ("api.open-meteo.com"), optionally "*." for its subdomains. Never an IP address,
// "localhost" or a single label (the local network is out of reach).
[[nodiscard]] bool isValidNetworkDomain(std::string_view domain) noexcept;
// Whether `host` (from a URL, any case) is one of `domains` or a subdomain of a "*." entry.
[[nodiscard]] bool networkDomainAllowed(std::string_view host, std::span<const std::string> domains) noexcept;

// A plugin's own action ("focus" in "io.github.someone.focus-mode:focus"), widget property or
// callback name: lower-case [a-z][a-z0-9-]*, no "--", at most 32 bytes.
[[nodiscard]] bool isValidPluginMemberName(std::string_view name) noexcept;

} // namespace rmw::domain
