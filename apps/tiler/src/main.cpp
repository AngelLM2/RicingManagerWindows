// rmw-tiler.exe — tiling, workspaces and their shortcuts (docs/14 §M13). A worker the core supervises:
//   rmw-tiler --plan <data>\tiler\plan.json [--heartbeat <handle>] [--core <handle>]
// It moves other apps' windows from the outside (nothing is injected) and puts every one back when
// it ends: when the plan goes, when the core goes, when the session ends. A crash leaves
// <data>\tiler\windows.json, which the next start (or the core) puts back first:
//   rmw-tiler --restore <data>\tiler\windows.json
// Advanced mode ([tiling] run_as_admin, ADR-0020): the supervised tiler asks Windows (the UAC) to
// start an elevated copy, which does the work and ends with it; the supervised one keeps beating:
//   rmw-tiler --plan <data>\tiler\plan.json --elevated-by <pid of the supervised one>

#include "AdminChoice.hpp"
#include "TilerApp.hpp"
#include "WindowOps.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"
#include "rmw/platform/Supervisor.hpp"
#include "rmw/tiling/Plan.hpp"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <string_view>
#include <vector>

namespace
{

using namespace rmw;
using tilerapp::TilerApp;

constexpr DWORD kBeatMs = 250;
constexpr int kPlainExit = 3; // the elevated copy left because the plan no longer asks for it
constexpr auto kQuickPlain = std::chrono::seconds(5);
constexpr std::uint64_t kMaxPlanBytes = std::uint64_t{256} << 10;

void startFileLog(const std::filesystem::path& dataDirectory, bool elevated)
{
    log::RotatingFileOptions options;
    // Two processes (advanced mode) never share a log file.
    options.path = dataDirectory / L"logs" / (elevated ? L"tiler-admin.log" : L"tiler.log");
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

std::filesystem::path pathArgument(std::span<const std::wstring_view> args, std::wstring_view name)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == name)
        {
            return {args[i + 1]};
        }
    }
    return {};
}

std::optional<std::uintptr_t> numberArgument(std::span<const std::wstring_view> args, std::wstring_view name)
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
                return value;
            }
        }
    }
    return std::nullopt;
}

// Whether the plan needs the elevated copy: advanced mode while tiling (TilerApp::applyPlan asks the same);
// nullopt when there is no (valid) plan. Advanced mode with tiling off asked the UAC again and again: the
// copy saw tiling off and left at once.
std::optional<bool> wantsAdmin(const std::filesystem::path& planFile)
{
    auto bytes = platform::readFileLimited(planFile, kMaxPlanBytes);
    if (!bytes)
    {
        return std::nullopt;
    }
    auto plan = tiling::parsePlan(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    return plan ? std::optional(plan->settings.enabled && plan->settings.runAsAdmin) : std::nullopt;
}

enum class Elevated : std::uint8_t
{
    Done,     // it ended (the plan went, the core went): everything is back
    Failed,   // it crashed
    Declined, // the person said no to the UAC prompt (or it could not be asked)
    Plain,    // the plan no longer asks for advanced mode
};

// Advanced mode: the UAC asks the person; the elevated copy does the work while this one beats for
// the supervisor. It ends with this process, so whatever ends this one ends it too.
Elevated runElevated(const std::filesystem::path& planFile, platform::Heartbeat& heartbeat, HANDLE core)
{
    auto self = platform::currentExecutable();
    if (!self)
    {
        return Elevated::Declined;
    }
    const std::wstring executable = self->native();
    const std::wstring parameters =
        std::format(L"--plan \"{}\" --elevated-by {}", planFile.native(), GetCurrentProcessId());
    // The prompt waits for the person as long as it takes: asked from a thread of its own, so the
    // heartbeat goes on (a silent worker would be taken for hung, ended, and asked again).
    struct Asked
    {
        std::wstring executable;
        std::wstring parameters;
        HANDLE process = nullptr;
        DWORD error = 0;
    };
    auto asked = std::make_shared<Asked>(Asked{executable, parameters, nullptr, 0});
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr)
    {
        return Elevated::Declined;
    }
    std::thread([asked, done] {
        try
        {
            if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
            {
                asked->error = ERROR_NOT_SUPPORTED;
            }
            else
            {
                SHELLEXECUTEINFOW info{};
                info.cbSize = sizeof(info);
                info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
                info.lpVerb = L"runas";
                info.lpFile = asked->executable.c_str();
                info.lpParameters = asked->parameters.c_str();
                info.nShow = SW_HIDE;
                if (ShellExecuteExW(&info) != FALSE && info.hProcess != nullptr)
                {
                    asked->process = info.hProcess;
                }
                else
                {
                    asked->error = GetLastError();
                }
                CoUninitialize();
            }
        }
        catch (...)
        {
            asked->error = ERROR_GEN_FAILURE;
        }
        SetEvent(done);
    }).detach();
    std::array<HANDLE, 2> handles = {done, core};
    const DWORD count = core != nullptr ? 2 : 1;
    for (;;)
    {
        const DWORD woke = WaitForMultipleObjects(count, handles.data(), FALSE, kBeatMs);
        heartbeat.beat();
        if (woke == WAIT_OBJECT_0 + 1)
        {
            // The core is gone. A copy started after this from a prompt still up finds this process
            // gone and leaves at once.
            log::info("tiler: the core is gone while Windows was asking for administrator");
            return Elevated::Done; // (the event stays with the thread, which may still be asking)
        }
        if (woke == WAIT_OBJECT_0)
        {
            break;
        }
    }
    CloseHandle(done);
    if (asked->process == nullptr)
    {
        if (asked->error == ERROR_CANCELLED)
        {
            log::info("tiler: administrator declined; the windows of apps running as administrator stay out");
        }
        else
        {
            log::warn("tiler: cannot run as administrator ({}); the windows of apps running as administrator "
                      "stay out",
                      asked->error);
        }
        return Elevated::Declined;
    }
    HANDLE child = asked->process;
    log::info("tiler: an elevated copy does the work (pid {})", GetProcessId(child));
    handles = {child, core};
    for (;;)
    {
        const DWORD woke = WaitForMultipleObjects(count, handles.data(), FALSE, kBeatMs);
        heartbeat.beat();
        if (woke == WAIT_OBJECT_0)
        {
            DWORD code = 1;
            GetExitCodeProcess(child, &code);
            CloseHandle(child);
            log::info("tiler: the elevated copy ended ({})", code);
            return code == 0 ? Elevated::Done : code == kPlainExit ? Elevated::Plain : Elevated::Failed;
        }
        if (woke == WAIT_OBJECT_0 + 1)
        {
            // It sees this process end and puts every window back.
            CloseHandle(child);
            log::info("tiler: the core is gone; the elevated copy puts every window back");
            return Elevated::Done;
        }
    }
}

int run(std::span<const std::wstring_view> args)
{
    // Window coordinates in physical pixels on every monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool elevated = tilerapp::runsAsAdmin();
    if (const auto file = pathArgument(args, L"--restore"); !file.empty())
    {
        startFileLog(file.parent_path().parent_path(), elevated);
        tilerapp::putBackFromFile(file);
        return 0;
    }
    const std::filesystem::path planFile = pathArgument(args, L"--plan");
    if (planFile.empty())
    {
        return 2;
    }
    startFileLog(planFile.parent_path().parent_path(), elevated);
    auto heartbeat = platform::Heartbeat::fromArguments(args);
    if (!heartbeat)
    {
        log::error("tiler: {}", heartbeat.error().toString());
        return 2;
    }
    // What ends this tiler: the core, or (the elevated copy) the tiler that started it.
    HANDLE ender = nullptr;
    const auto parent = numberArgument(args, L"--elevated-by");
    if (parent)
    {
        ender = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(*parent));
        if (ender == nullptr)
        {
            return 0; // it is gone already
        }
    }
    else if (const auto core = numberArgument(args, L"--core"))
    {
        ender = reinterpret_cast<HANDLE>(*core); // NOLINT(performance-no-int-to-ptr): inherited
    }
    // A copy that did not get the rights it was started for (Windows with the UAC off runs "as
    // administrator" without them) works as it is: asking again would start copies without end.
    bool declined = parent.has_value() && !elevated;
    if (declined)
    {
        log::warn("tiler: started to run as administrator, but it is not; the windows of apps running as "
                  "administrator stay out");
    }
    else if (!parent && !elevated && tilerapp::adminDeclinedThisSession(planFile))
    {
        // A tiler started again (after a crash, by a core started again) does not ask a second time.
        log::info("tiler: administrator was declined earlier in this sign-in; not asked again until the "
                  "advanced mode is turned off and on");
        declined = true;
    }
    // Windows' virtual desktops (the Mac style) are a COM interface.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    int code = 0;
    for (;;)
    {
        const auto admin = wantsAdmin(planFile);
        if (!admin)
        {
            break; // no plan: nothing to do
        }
        if (!*admin && !parent && !elevated)
        {
            // Advanced mode (or tiling) off: turning it on asks once more.
            tilerapp::forgetAdminDeclined(planFile);
        }
        if (*admin && !elevated && !declined)
        {
            const auto asked = std::chrono::steady_clock::now();
            const Elevated outcome = runElevated(planFile, *heartbeat, ender);
            if (outcome == Elevated::Declined)
            {
                declined = true; // not asked again in this sign-in (see AdminChoice)
                tilerapp::rememberAdminDeclined(planFile);
                continue;
            }
            if (outcome == Elevated::Plain)
            {
                // The copy left at once although the plan still asks for it: never a second prompt for that.
                if (std::chrono::steady_clock::now() - asked < kQuickPlain && wantsAdmin(planFile).value_or(false))
                {
                    log::warn("tiler: the elevated copy left at once; not asking for administrator again");
                    declined = true;
                    tilerapp::rememberAdminDeclined(planFile);
                }
                continue;
            }
            if (outcome == Elevated::Failed)
            {
                // Ending this tiler with an error would have the supervisor start it again, and it would
                // ask again: the plain one works instead.
                log::warn("tiler: the elevated copy failed; working without administrator in this sign-in");
                declined = true;
                tilerapp::rememberAdminDeclined(planFile);
                continue;
            }
            code = 0;
            break;
        }
        const TilerApp::Ending ending = [&] {
            TilerApp app(planFile, *heartbeat, ender, {elevated, declined, parent.has_value()});
            return app.run();
        }();
        if (ending == TilerApp::Ending::BeAdmin)
        {
            declined = false; // the advanced mode was turned off and on: asked once more
            continue;
        }
        code = ending == TilerApp::Ending::BePlain ? kPlainExit : ending == TilerApp::Ending::Failed ? 1 : 0;
        break;
    }
    if (com)
    {
        CoUninitialize();
    }
    if (parent && ender != nullptr)
    {
        CloseHandle(ender);
    }
    log::info("tiler: done");
    return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): the entry point
{
    try
    {
        if (auto hardened = rmw::platform::hardenProcess(); !hardened)
        {
            rmw::log::warn("tiler: {}", hardened.error().toString());
        }
        if (auto mitigated = rmw::platform::applyStrictMitigations(); !mitigated)
        {
            rmw::log::warn("tiler: some process mitigations are off: {}", mitigated.error().toString());
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
        rmw::log::error("tiler: fatal: {}", e.what());
    }
    catch (...)
    {
        rmw::log::error("tiler: fatal: unknown exception");
    }
    return 1; // the next start (or the core) puts the windows back from windows.json
}
