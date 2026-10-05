// rmw-bar.exe — the custom taskbar (docs/04 M3). A worker the core supervises:
//   rmw-bar --plan <data>\bar\bars.json [--heartbeat <handle>] [--core <handle>]
// It never runs plugin code (docs/12 §3). When the core goes away it shows Windows' taskbar again
// and exits, so a crash of the core never leaves the user without a taskbar.

#include "BarApp.hpp"
#include "SlintPlatform.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/ProcessHardening.hpp"
#include "rmw/platform/Supervisor.hpp"

#include <windows.h>

#include <roapi.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using namespace rmw;
using namespace std::chrono_literals;

void startFileLog(const std::filesystem::path& dataDirectory)
{
    log::RotatingFileOptions options;
    options.path = dataDirectory / L"logs" / L"bar.log";
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

HANDLE handleArgument(std::span<const std::wstring_view> args, std::wstring_view name)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == name)
        {
            const std::string text = utf16To8Lossy(args[i + 1]);
            std::uintptr_t value = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec == std::errc{} && ptr == text.data() + text.size())
            {
                return reinterpret_cast<HANDLE>(value); // NOLINT(performance-no-int-to-ptr): inherited
            }
        }
    }
    return nullptr;
}

int run(std::span<const std::wstring_view> args)
{
    std::filesystem::path planFile;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == L"--plan")
        {
            planFile = std::filesystem::path(args[i + 1]);
        }
    }
    if (planFile.empty())
    {
        return 2;
    }
    startFileLog(planFile.parent_path().parent_path());

    auto heartbeat = platform::Heartbeat::fromArguments(args);
    if (!heartbeat)
    {
        log::error("bar: {}", heartbeat.error().toString());
        return 2;
    }
    // Per-monitor DPI: every bar is sized for its own monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The UI thread is a single-threaded apartment: the volume (Core Audio) and media (WinRT)
    // widgets talk COM from it.
    if (FAILED(RoInitialize(RO_INIT_SINGLETHREADED)))
    {
        log::warn("bar: COM is not available; the volume and media widgets stay empty");
    }

    barapp::BarApp* app = nullptr;
    auto platform = std::make_unique<barapp::Win32Platform>([&app] { return app->createAdapter(); });
    barapp::Win32Platform& loop = *platform;
    slint::platform::set_platform(std::move(platform));
    loop.everyTurn([&heartbeat] { heartbeat->beat(); }, 500ms);
    if (auto* const core = handleArgument(args, L"--core"))
    {
        loop.watch(core, [] {
            log::info("bar: the core is gone; leaving");
            slint::quit_event_loop();
        });
    }

    barapp::BarApp bar(loop, planFile);
    app = &bar;
    if (!bar.start())
    {
        log::info("bar: nothing to show");
        return 0;
    }
    log::info("bar: running");
    // Until asked: rebuilding hides every bar for a moment, which must not end the loop.
    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
    log::info("bar: stopped");
    return 0; // ~BarApp shows the native taskbar again
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
        std::vector<std::wstring_view> args;
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }
        return run(args);
    }
    catch (const std::exception& e)
    {
        rmw::log::error("bar: fatal: {}", e.what());
    }
    catch (...)
    {
        rmw::log::error("bar: fatal: unknown exception");
    }
    rmw::platform::showNativeTaskbars();
    return 1;
}
