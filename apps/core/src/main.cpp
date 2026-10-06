// rmw-core.exe — the single writer (docs/03 §2). Modes:
//   rmw-core [--data-dir <path>] [--in-process] [--idle-exit <seconds>]   serve clients; stays
//                                                                          resident if needed
//   rmw-core --startup        at logon: undo an interrupted operation, stay only if needed
//   rmw-core --safe-mode      nothing automatic (also: Shift held at logon, or 3 crashed starts)
//   rmw-core --apply-helper --pipe <name>                                  child that talks to Explorer
//   rmw-core --import-worker --kind rice|plugin --archive <file> --staging <folder> --report <file>
//                                                                          child that unpacks a package
//   rmw-core --version

#include "ApplyHelper.hpp"
#include "CoreServer.hpp"
#include "Import.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/platform/Console.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"

#include <windows.h>

#include <charconv>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace rmw;

struct Arguments
{
    bool version = false;
    bool startup = false;
    bool applyHelper = false;
    bool importWorker = false;
    std::wstring importKind;
    std::filesystem::path importArchive;
    std::filesystem::path importStaging;
    std::filesystem::path importReport;
    bool inProcess = false;
    bool safeMode = false;
    bool testExitUnclean = false;
    std::wstring pipe;
    std::filesystem::path dataDirectory;
    std::optional<int> idleExit;
    int testDelayMs = 0;
    std::string testPauseModule;
    int testPauseMs = 0;
};

Result<int> parseMilliseconds(std::wstring_view text)
{
    const std::string narrow = utf16To8Lossy(text);
    int value = 0;
    const auto [ptr, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
    if (ec != std::errc{} || ptr != narrow.data() + narrow.size() || value < 0 || value > 600'000)
    {
        return fail(ErrorCode::InvalidArgument, "expected milliseconds (0..600000)");
    }
    return value;
}

Result<Arguments> parseArguments(std::span<wchar_t*> args)
{
    Arguments parsed;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring_view arg = args[i];
        const auto value = [&]() -> Result<std::wstring> {
            if (i + 1 >= args.size())
            {
                return fail(ErrorCode::InvalidArgument, std::format("{} needs a value", utf16To8Lossy(arg)));
            }
            return std::wstring(args[++i]);
        };
        if (arg == L"--version")
        {
            parsed.version = true;
        }
        else if (arg == L"--startup")
        {
            parsed.startup = true;
        }
        else if (arg == L"--apply-helper")
        {
            parsed.applyHelper = true;
        }
        else if (arg == L"--import-worker")
        {
            parsed.importWorker = true;
        }
        else if (arg == L"--kind" || arg == L"--archive" || arg == L"--staging" || arg == L"--report")
        {
            auto text = value();
            if (!text)
            {
                return std::unexpected(std::move(text.error()));
            }
            if (arg == L"--kind")
            {
                parsed.importKind = std::move(*text);
            }
            else if (arg == L"--archive")
            {
                parsed.importArchive = std::move(*text);
            }
            else if (arg == L"--staging")
            {
                parsed.importStaging = std::move(*text);
            }
            else
            {
                parsed.importReport = std::move(*text);
            }
        }
        else if (arg == L"--in-process")
        {
            parsed.inProcess = true;
        }
        else if (arg == L"--safe-mode")
        {
            parsed.safeMode = true;
        }
        else if (arg == L"--test-exit-unclean")
        {
            parsed.testExitUnclean = true; // system tests only: die like a crash after starting
        }
        else if (arg == L"--test-delay-ms" || arg == L"--test-pause-after")
        {
            // System tests only: --test-delay-ms <ms>, --test-pause-after <module>:<ms>
            auto text = value();
            if (!text)
            {
                return std::unexpected(std::move(text.error()));
            }
            if (arg == L"--test-delay-ms")
            {
                auto ms = parseMilliseconds(*text);
                if (!ms)
                {
                    return std::unexpected(std::move(ms.error()));
                }
                parsed.testDelayMs = *ms;
            }
            else
            {
                const auto colon = text->find(L':');
                auto ms = colon == std::wstring::npos
                              ? Result<int>(fail(ErrorCode::InvalidArgument, "expected <module>:<ms>"))
                              : parseMilliseconds(std::wstring_view(*text).substr(colon + 1));
                if (!ms)
                {
                    return std::unexpected(std::move(ms.error()));
                }
                parsed.testPauseModule = utf16To8Lossy(std::wstring_view(*text).substr(0, colon));
                parsed.testPauseMs = *ms;
            }
        }
        else if (arg == L"--pipe" || arg == L"--data-dir" || arg == L"--idle-exit")
        {
            auto text = value();
            if (!text)
            {
                return std::unexpected(std::move(text.error()));
            }
            if (arg == L"--pipe")
            {
                parsed.pipe = std::move(*text);
            }
            else if (arg == L"--data-dir")
            {
                parsed.dataDirectory = std::move(*text);
            }
            else
            {
                const std::string narrow = utf16To8Lossy(*text);
                int seconds = 0;
                const auto [ptr, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), seconds);
                if (ec != std::errc{} || ptr != narrow.data() + narrow.size() || seconds < 1 ||
                    seconds > 86'400)
                {
                    return fail(ErrorCode::InvalidArgument, "--idle-exit takes 1..86400 seconds");
                }
                parsed.idleExit = seconds;
            }
        }
        else
        {
            return fail(ErrorCode::InvalidArgument, std::format("unknown argument '{}'", utf16To8Lossy(arg)));
        }
    }
    return parsed;
}

void startFileLog(const std::filesystem::path& dataDirectory, std::wstring_view name)
{
    log::RotatingFileOptions options;
    options.path = dataDirectory / L"logs" / (std::wstring(name) + L".log");
    if (auto sink = log::makeRotatingFileSink(std::move(options)))
    {
        log::instance().addSink(std::move(*sink));
    }
    // The user's profile path never lands in a log (docs/06 §8).
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

int run(std::span<wchar_t*> args)
{
    auto parsed = parseArguments(args);
    if (!parsed)
    {
        platform::writeLine(platform::Stream::Err, "rmw-core: " + parsed.error().message());
        return 2;
    }
    if (parsed->version)
    {
        platform::writeLine(platform::Stream::Out, std::format("rmw-core {}", kVersion));
        return 0;
    }

    std::filesystem::path dataDirectory = parsed->dataDirectory;
    if (dataDirectory.empty())
    {
        auto defaultDirectory = platform::defaultDataDirectory();
        if (!defaultDirectory)
        {
            platform::writeLine(platform::Stream::Err, "rmw-core: " + defaultDirectory.error().toString());
            return 1;
        }
        dataDirectory = std::move(*defaultDirectory);
    }
    startFileLog(dataDirectory, parsed->applyHelper    ? L"apply-helper"
                                : parsed->importWorker ? L"import-worker"
                                                       : L"core");

    // ADR-0002: no foreign code inside the core or its helper from here on.
    if (auto mitigated = platform::applyStrictMitigations(); !mitigated)
    {
        log::warn("process mitigations not applied: {}", mitigated.error().toString());
    }

    if (parsed->applyHelper)
    {
        if (parsed->pipe.empty())
        {
            log::error("--apply-helper needs --pipe");
            return 2;
        }
        return core::runApplyHelper(parsed->pipe);
    }
    if (parsed->importWorker)
    {
        if (parsed->importArchive.empty() || parsed->importStaging.empty() || parsed->importReport.empty())
        {
            log::error("--import-worker needs --kind, --archive, --staging and --report");
            return 2;
        }
        return core::runImportWorker(parsed->importKind, parsed->importArchive, parsed->importStaging,
                                     parsed->importReport);
    }

    auto instance = platform::SingleInstance::acquire(L"core");
    if (!instance)
    {
        log::error("single instance: {}", instance.error().toString());
        return 1;
    }
    if (!instance->owned())
    {
        log::info("another core is already running");
        return 0;
    }

    core::CoreOptions options;
    options.dataDirectory = dataDirectory;
    options.inProcess = parsed->inProcess;
    if (parsed->idleExit)
    {
        options.idleExit = std::chrono::seconds(*parsed->idleExit);
    }
    options.startup = parsed->startup;
    // Holding Shift while Windows signs in starts in safe mode (docs/06 §6). Only at logon: a
    // client starting the core while the user types must not trigger it.
    options.safeModeRequested =
        parsed->safeMode || (parsed->startup && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    options.testOperationDelay = std::chrono::milliseconds(parsed->testDelayMs);
    options.testPauseAfterModule = parsed->testPauseModule;
    options.testPause = std::chrono::milliseconds(parsed->testPauseMs);
    core::CoreServer server(std::move(options));
    if (auto initialized = server.initialize(); !initialized)
    {
        log::error("core: {}", initialized.error().toString());
        return 1;
    }
    if (parsed->testExitUnclean)
    {
        log::warn("TEST HOOK: exiting like a crash");
        std::_Exit(70);
    }
    if (parsed->startup && !server.wantsResidency())
    {
        // initialize() already undid any interrupted operation; nothing needs us (docs/03 §2.2).
        log::info("startup: nothing needs the core; exiting");
        server.markHealthy();
        return 0;
    }

    auto name = platform::corePipeName();
    if (!name)
    {
        log::error("core pipe name: {}", name.error().toString());
        return 1;
    }
    auto pipe = platform::PipeServer::create(*name);
    if (!pipe)
    {
        log::error("core pipe: {}", pipe.error().toString());
        if (pipe.error().code() == ErrorCode::AlreadyExists)
        {
            server.markHealthy();
            return 0;
        }
        return 1;
    }
    return server.run(*pipe);
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    // Process boundary: nothing may escape as an exception (docs/08 §3 rule 5).
    try
    {
        if (auto hardened = rmw::platform::hardenProcess(); !hardened)
        {
            rmw::log::warn("hardening: {}", hardened.error().toString());
        }
        return run(std::span(argv + 1, static_cast<std::size_t>(argc > 1 ? argc - 1 : 0)));
    }
    catch (const std::exception& e)
    {
        rmw::log::error("fatal: {}", e.what());
    }
    catch (...)
    {
        rmw::log::error("fatal: unknown exception");
    }
    return 1;
}
