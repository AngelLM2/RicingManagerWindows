// rmw-plugin-host.exe — runs the community's Luau plugins (docs/12 §3, P2). A worker the core
// supervises, in a job with a memory ceiling, no child processes and no clipboard:
//   rmw-plugin-host --data <dir> [--bridge <pipe>] [--control <pipe>] [--blame <handle>]
//                   --heartbeat <handle>
// The bar and the core never run plugin code; this process does, and only through the sandbox of
// rmw::scripting.

#include "HostApp.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"
#include "rmw/platform/Supervisor.hpp"

#include <windows.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using namespace rmw;

void startFileLog(const std::filesystem::path& dataDirectory)
{
    log::RotatingFileOptions options;
    options.path = dataDirectory / L"logs" / L"plugins.log";
    if (auto sink = log::makeRotatingFileSink(std::move(options)))
    {
        log::instance().addSink(std::move(*sink));
    }
    wchar_t* raw = nullptr;
    std::size_t length = 0;
    const errno_t found = _wdupenv_s(&raw, &length, L"USERPROFILE");
    const std::unique_ptr<wchar_t, decltype(&std::free)> profile(raw, &std::free);
    if (found == 0 && profile)
    {
        log::instance().addRedaction(utf16To8Lossy(profile.get()), "%USERPROFILE%");
    }
    log::instance().setLevel(log::Level::Info);
}

std::wstring_view argument(std::span<const std::wstring_view> args, std::wstring_view name)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == name)
        {
            return args[i + 1];
        }
    }
    return {};
}

void* handleArgument(std::span<const std::wstring_view> args, std::wstring_view name)
{
    const std::string text = utf16To8Lossy(argument(args, name));
    std::uintptr_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size())
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(value); // NOLINT(performance-no-int-to-ptr): inherited
}

int run(std::span<const std::wstring_view> args)
{
    pluginhost::HostOptions options;
    options.dataDirectory = std::filesystem::path(argument(args, L"--data"));
    if (options.dataDirectory.empty() || !options.dataDirectory.is_absolute())
    {
        return 2;
    }
    options.planFile = options.dataDirectory / L"plugin-host" / L"plan.json";
    options.bridgePipe = std::wstring(argument(args, L"--bridge"));
    options.controlPipe = std::wstring(argument(args, L"--control"));
    options.blame = handleArgument(args, L"--blame");
    startFileLog(options.dataDirectory);

    auto heartbeat = platform::Heartbeat::fromArguments(args);
    if (!heartbeat)
    {
        log::error("plugin host: {}", heartbeat.error().toString());
        return 2;
    }
    log::info("plugin host: running");
    pluginhost::HostApp app(std::move(options), *heartbeat);
    const int code = app.run();
    log::info("plugin host: stopped");
    return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    try
    {
        if (auto hardened = rmw::platform::hardenProcess(); !hardened)
        {
            rmw::log::warn("hardening: {}", hardened.error().toString());
        }
        // No JIT in Luau: the strict policies (no dynamic code, Microsoft-signed DLLs only) hold.
        if (auto strict = rmw::platform::applyStrictMitigations(); !strict)
        {
            rmw::log::warn("mitigations: {}", strict.error().toString());
        }
        std::vector<std::wstring_view> args;
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }
        return run(args);
    }
    catch (const std::exception& e)
    {
        rmw::log::error("plugin host: fatal: {}", e.what());
    }
    catch (...)
    {
        rmw::log::error("plugin host: fatal: unknown exception");
    }
    return 1;
}
