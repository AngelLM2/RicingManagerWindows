// rmw-wallpaper.exe — video wallpapers (docs/04 M6). A worker the core supervises:
//   rmw-wallpaper --plan <data>\wallpaper\video.json [--heartbeat <handle>]
// It plays the plan's videos behind the desktop icons; Windows' own wallpaper (the rice's fallback
// image) stays underneath, so whatever happens to this process the desktop is never left blank.

#include "WallpaperApp.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"
#include "rmw/platform/Supervisor.hpp"

#include <windows.h>

#include <mfapi.h>

#include <objbase.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using namespace rmw;

void startFileLog(const std::filesystem::path& dataDirectory)
{
    log::RotatingFileOptions options;
    options.path = dataDirectory / L"logs" / L"wallpaper.log";
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
        log::error("wallpaper: {}", heartbeat.error().toString());
        return 2;
    }
    // Every monitor at its own resolution.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) ||
        FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        log::error("wallpaper: Media Foundation is not available on this Windows");
        return 1;
    }
    int code = 0;
    {
        wallpaper::WallpaperApp app(planFile, std::move(*heartbeat));
        code = app.run();
    }
    MFShutdown();
    CoUninitialize();
    log::info("wallpaper: done");
    return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): the entry point
{
    if (auto hardened = rmw::platform::hardenProcess(); !hardened)
    {
        rmw::log::warn("wallpaper: {}", hardened.error().toString());
    }
    // Not the core's strict set (ADR-0002): GPU drivers and decoders load DLLs that are not
    // Microsoft's and may generate code. What stays: no extension points, no remote or
    // low-integrity images, system DLLs first.
    if (auto mitigated = rmw::platform::applyMediaMitigations(); !mitigated)
    {
        rmw::log::warn("wallpaper: some process mitigations are off: {}", mitigated.error().toString());
    }
    std::vector<std::wstring_view> args(argv, argv + argc);
    return run(args);
}
