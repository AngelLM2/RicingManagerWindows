#include "rmw/domain/PluginApi.hpp"

#include <algorithm>
#include <array>

namespace rmw::domain
{
namespace
{

constexpr auto kProviders = std::to_array<PluginCapability>({
    {"time", false, "read the local time"},
    {"system.cpu", false, "read the CPU usage"},
    {"system.memory", false, "read the memory usage"},
    {"network.stats", false, "read how much the network sends and receives"},
    {"battery", false, "read the battery level and whether the PC is plugged in"},
    {"audio.volume", false, "read the speaker volume"},
    {"monitors", false, "read the list of monitors"},
    {"virtual-desktops", false, "read how many virtual desktops there are and which one is shown"},
    {"media", true, "see what is playing (title, artist, app)"},
    {"windows.list", true, "see the titles of your open windows"},
});

constexpr auto kActions = std::to_array<PluginCapability>({
    {"media.control", false, "play, pause and skip what is playing"},
    {"window.focus", false, "bring a window to the front"},
    {"window.minimize", false, "minimize a window"},
    {"window.close", true, "close your windows"},
    {"rice.apply", false, "apply a rice that comes with the plugin"},
    {"rice.undo", false, "undo the last change of rice"},
    {"wallpaper.next", false, "show the next wallpaper of a slideshow"},
    {"bar.toggle", false, "show or hide the bar"},
    {"bar.on", false, "show the bar"},
    {"bar.off", false, "hide the bar"},
    {"appearance.toggle", false, "switch between light and dark mode"},
    {"appearance.dark", false, "switch to dark mode"},
    {"appearance.light", false, "switch to light mode"},
});

// Names that only make sense on a local network (RFC 6761, RFC 8375 and common practice).
constexpr auto kLocalSuffixes = std::to_array<std::string_view>({
    "localhost",
    "local",
    "lan",
    "home",
    "internal",
    "intranet",
    "corp",
    "arpa",
    "test",
    "invalid",
});

const PluginCapability* findIn(std::span<const PluginCapability> list, std::string_view name) noexcept
{
    const auto found = std::ranges::find(list, name, &PluginCapability::name);
    return found == list.end() ? nullptr : &*found;
}

bool isLabelChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::ranges::equal(a, b, {}, lower, lower);
}

} // namespace

SemVer pluginApiVersion() noexcept
{
    return SemVer{1, 0, 0, {}};
}

std::span<const PluginCapability> pluginProviders() noexcept
{
    return kProviders;
}

std::span<const PluginCapability> pluginActions() noexcept
{
    return kActions;
}

const PluginCapability* findPluginProvider(std::string_view name) noexcept
{
    return findIn(kProviders, name);
}

const PluginCapability* findPluginAction(std::string_view name) noexcept
{
    return findIn(kActions, name);
}

bool isValidNetworkDomain(std::string_view domain) noexcept
{
    if (domain.starts_with("*."))
    {
        domain.remove_prefix(2);
    }
    if (domain.empty() || domain.size() > 253)
    {
        return false;
    }
    std::size_t labels = 0;
    std::string_view last;
    std::size_t start = 0;
    while (start <= domain.size())
    {
        const std::size_t dot = std::min(domain.find('.', start), domain.size());
        const std::string_view label = domain.substr(start, dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::ranges::all_of(label, isLabelChar))
        {
            return false;
        }
        ++labels;
        last = label;
        start = dot + 1;
    }
    // An all-digit top-level label means an IPv4 address ("192.168.0.1").
    const bool numericTop = std::ranges::all_of(last, [](char c) { return c >= '0' && c <= '9'; });
    return labels >= 2 && !numericTop && std::ranges::find(kLocalSuffixes, last) == kLocalSuffixes.end();
}

bool networkDomainAllowed(std::string_view host, std::span<const std::string> domains) noexcept
{
    if (host.ends_with('.'))
    {
        host.remove_suffix(1); // "example.com." is the same name
    }
    if (host.empty())
    {
        return false;
    }
    for (const std::string& entry : domains)
    {
        const std::string_view domain(entry);
        if (domain.starts_with("*."))
        {
            const std::string_view suffix = domain.substr(1); // ".example.com"
            if (host.size() > suffix.size() &&
                equalsIgnoringCase(host.substr(host.size() - suffix.size()), suffix))
            {
                return true;
            }
        }
        else if (equalsIgnoringCase(host, domain))
        {
            return true;
        }
    }
    return false;
}

bool isValidPluginMemberName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 32 || name.front() < 'a' || name.front() > 'z' || name.back() == '-' ||
        name.find("--") != std::string_view::npos)
    {
        return false;
    }
    return std::ranges::all_of(name, isLabelChar);
}

} // namespace rmw::domain
