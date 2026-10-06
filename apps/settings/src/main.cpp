// rmw-settings.exe — the settings and gallery of Ricing Manager (docs/04 M7):
//   rmw-settings [--data-dir <folder>]
// One window, one instance: starting it again brings the window to the front. A client of
// rmw-core (started when needed); closing the window ends the process and frees all it held.

#include "SettingsApp.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"

#include <windows.h>

#include <shellapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <objbase.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

namespace
{

using namespace rmw;

constexpr const wchar_t* kInstanceMutex = L"Local\\RicingManagerWindows.Settings";
constexpr const wchar_t* kShowEvent = L"Local\\RicingManagerWindows.Settings.Show";

void startFileLog(const std::filesystem::path& dataDirectory)
{
    log::RotatingFileOptions options;
    options.path = dataDirectory / L"logs" / L"settings.log";
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

std::filesystem::path dataDirectoryFrom(const wchar_t* commandLine)
{
    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> args(CommandLineToArgvW(commandLine, &count),
                                                             &LocalFree);
    for (int i = 0; args && i + 1 < count; ++i)
    {
        if (std::wstring_view(args.get()[i]) == L"--data-dir")
        {
            std::error_code ec;
            return std::filesystem::absolute(args.get()[i + 1], ec);
        }
    }
    auto standard = platform::defaultDataDirectory();
    return standard ? *standard : std::filesystem::path();
}

int run(HANDLE showEvent, HANDLE closeEvent)
{
    const auto dataDirectory = dataDirectoryFrom(GetCommandLineW());
    if (dataDirectory.empty())
    {
        return 1;
    }
    startFileLog(dataDirectory);
    // ADR-0007: the software renderer, chosen here (Slint's default renders on the GPU and costs
    // ~16 times the memory).
    SetEnvironmentVariableW(L"SLINT_BACKEND", L"winit-software");
    // File dialogs and the shell's thumbnails want a single-threaded apartment.
    const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);

    settingsapp::SettingsApp app(dataDirectory);
    std::jthread watcher;
    if (showEvent != nullptr && closeEvent != nullptr)
    {
        watcher = std::jthread([&app, showEvent, closeEvent](const std::stop_token& stop) {
            const std::array<HANDLE, 2> events = {showEvent, closeEvent};
            while (!stop.stop_requested())
            {
                const DWORD signaled =
                    WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, 250);
                if (signaled == WAIT_OBJECT_0)
                {
                    slint::invoke_from_event_loop([&app] { app.bringToFront(); });
                }
                else if (signaled == WAIT_OBJECT_0 + 1)
                {
                    app.closeForExit(); // at once: no call may start the core from here on
                    slint::invoke_from_event_loop([&app] { app.hideForExit(); });
                }
            }
        });
    }
    log::info("settings: open");
    app.run();
    log::info("settings: closed");
    return 0;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): process entry point
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // One window: started again, it brings the running one to the front and is gone at once (no
    // exit work: it started nothing, and the DLLs' teardown has nothing to do for it).
    const wil::unique_mutex_nothrow instance(CreateMutexW(nullptr, FALSE, kInstanceMutex));
    const bool first = instance && GetLastError() != ERROR_ALREADY_EXISTS;
    const wil::unique_event_nothrow show(CreateEventW(nullptr, FALSE, FALSE, kShowEvent));
    const wil::unique_event_nothrow close(CreateEventW(nullptr, FALSE, FALSE, rmw::platform::kSettingsCloseEvent));
    if (!first)
    {
        AllowSetForegroundWindow(ASFW_ANY);
        if (show)
        {
            SetEvent(show.get());
        }
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    if (auto hardened = rmw::platform::hardenProcess(); !hardened)
    {
        rmw::log::warn("hardening: {}", hardened.error().toString());
    }
    try
    {
        return run(show.get(), close.get());
    }
    catch (const std::exception& e)
    {
        rmw::log::error("settings: fatal: {}", e.what());
    }
    catch (...)
    {
        rmw::log::error("settings: fatal: unknown exception");
    }
    return 1;
}
