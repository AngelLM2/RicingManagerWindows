// rmw.exe — command-line client of rmw-core (docs/04 M1). English on purpose for v1.0.

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/Console.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"

#include <charconv>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace rmw;
using namespace std::chrono_literals;
using ipc::Json;

constexpr std::string_view kUsage = R"(Usage: rmw <command> [options]

Commands:
  apply <folder> [--wait]               Apply the rice in <folder> (reads <folder>\rice.toml)
  validate <folder>                     Check a rice without applying it
  status                                What is managed now, and the last operations
  history                               Operations you can go back to
  restore --original [--force]          Back to the Windows you had before the first apply
  restore --snapshot <id> [--force]     Back to how things were before operation <id>
  export <folder> [--name <name>]       Save the current look as a rice in a new folder
  monitors                              Connected monitors and their keys (for edid:<key>)
  inspect <module>                      What Windows has now for a module (appearance,
                                        taskbar, cursors, wallpaper), as JSON
  action <name> [--path <folder>]       Run an action: rice.apply, restore.original,
                                        appearance.toggle
  shutdown                              Stop the background core
  --version, --help

Restores skip settings you changed yourself after applying; --force restores them too.

Exit codes: 0 ok, 1 failed, 2 usage, 3 busy, 4 the rice has errors.)";

enum Exit : int
{
    Ok = 0,
    Failed = 1,
    Usage = 2,
    Busy = 3,
    InvalidRice = 4,
};

void out(std::string_view text)
{
    platform::writeLine(platform::Stream::Out, text);
}

void err(std::string_view text)
{
    platform::writeLine(platform::Stream::Err, text);
}

std::string timeText(std::int64_t unixSeconds)
{
    const auto time = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    if (localtime_s(&local, &time) != 0)
    {
        return std::to_string(unixSeconds);
    }
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

// ---- connection to the core ---------------------------------------------------------------------

class CoreClient
{
public:
    explicit CoreClient(std::filesystem::path dataDirectory)
        : m_dataDirectory(std::move(dataDirectory))
    {
    }

    Result<void> connect()
    {
        auto self = platform::currentExecutable();
        auto name = platform::corePipeName();
        if (!self || !name)
        {
            return fail(ErrorCode::System, "cannot locate the core");
        }
        m_coreExe = self->parent_path() / L"rmw-core.exe";
        m_pipe = *name;

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto connected = connectOrStart();
            if (!connected)
            {
                return std::unexpected(std::move(connected.error()));
            }
            auto hello = call("hello", ipc::helloParams(kVersion), 10s);
            if (hello)
            {
                return ipc::checkHello(*hello);
            }
            if (hello.error().code() != ErrorCode::Unsupported)
            {
                return std::unexpected(std::move(hello.error()));
            }
            // An older core is still running (e.g. after an update): replace it.
            [[maybe_unused]] const auto stopped = call("shutdown", Json::object(), 5s);
            m_connection.reset();
            std::this_thread::sleep_for(1s);
        }
        return fail(ErrorCode::Unsupported, "the running core is from another version and did not stop");
    }

    Result<Json> call(std::string_view method, const Json& params, std::chrono::milliseconds timeout)
    {
        const std::int64_t id = m_nextId++;
        if (auto sent = m_connection->send(ipc::requestText(id, method, params), 10s); !sent)
        {
            return std::unexpected(std::move(sent.error()));
        }
        auto reply = m_connection->receive(timeout);
        if (!reply)
        {
            return std::unexpected(std::move(reply.error()));
        }
        auto response = ipc::parseResponse(*reply);
        if (!response || response->id != id)
        {
            return fail(ErrorCode::InvalidData, "unexpected reply from the core");
        }
        if (response->error)
        {
            return std::unexpected(ipc::fromRpcError(*response->error));
        }
        return std::move(response->result);
    }

private:
    Result<void> connectOrStart()
    {
        auto connection = platform::connectPipe(m_pipe, 2s, m_coreExe);
        if (!connection && connection.error().code() == ErrorCode::NotFound)
        {
            std::wstring arguments;
            if (!m_dataDirectory.empty())
            {
                arguments = L"--data-dir \"" + m_dataDirectory.native() + L"\"";
            }
            // Outside the terminal's job: closing the terminal must not kill the core.
            auto core = platform::ChildProcess::spawn(m_coreExe, arguments,
                                                      {.breakawayFromJob = true, .ownJob = false});
            if (!core)
            {
                return std::unexpected(std::move(core.error()).withContext("start rmw-core"));
            }
            core->detach();
            for (int i = 0; i < 50; ++i)
            {
                connection = platform::connectPipe(m_pipe, 2s, m_coreExe);
                if (connection || connection.error().code() != ErrorCode::NotFound)
                {
                    break;
                }
                std::this_thread::sleep_for(200ms);
            }
        }
        if (!connection)
        {
            return std::unexpected(std::move(connection.error()).withContext("connect to rmw-core"));
        }
        m_connection = std::make_unique<platform::PipeConnection>(std::move(*connection));
        return {};
    }

    std::filesystem::path m_dataDirectory;
    std::filesystem::path m_coreExe;
    std::wstring m_pipe;
    std::unique_ptr<platform::PipeConnection> m_connection;
    std::int64_t m_nextId = 1;
};

// ---- commands -------------------------------------------------------------------------------------

struct Options
{
    std::vector<std::string> positional;
    bool wait = false;
    bool force = false;
    bool original = false;
    std::optional<std::int64_t> snapshot;
    std::string path;
    std::string name;
    std::filesystem::path dataDirectory;
};

Result<Options> parseOptions(std::span<const std::string> args)
{
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        const auto value = [&]() -> Result<std::string> {
            if (i + 1 >= args.size())
            {
                return fail(ErrorCode::InvalidArgument, std::format("{} needs a value", arg));
            }
            return args[++i];
        };
        if (arg == "--wait")
        {
            options.wait = true;
        }
        else if (arg == "--force")
        {
            options.force = true;
        }
        else if (arg == "--original")
        {
            options.original = true;
        }
        else if (arg == "--snapshot" || arg == "--path" || arg == "--data-dir" || arg == "--name")
        {
            auto text = value();
            if (!text)
            {
                return std::unexpected(std::move(text.error()));
            }
            if (arg == "--snapshot")
            {
                std::int64_t id = 0;
                const auto [ptr, ec] = std::from_chars(text->data(), text->data() + text->size(), id);
                if (ec != std::errc{} || ptr != text->data() + text->size() || id <= 0)
                {
                    return fail(ErrorCode::InvalidArgument, "--snapshot takes the id shown by 'rmw history'");
                }
                options.snapshot = id;
            }
            else if (arg == "--path")
            {
                options.path = std::move(*text);
            }
            else if (arg == "--name")
            {
                options.name = std::move(*text);
            }
            else
            {
                auto wide = utf8To16(*text);
                if (!wide)
                {
                    return std::unexpected(std::move(wide.error()));
                }
                options.dataDirectory = std::filesystem::absolute(*wide);
            }
        }
        else if (arg.starts_with("--"))
        {
            return fail(ErrorCode::InvalidArgument, std::format("unknown option '{}'", arg));
        }
        else
        {
            options.positional.push_back(arg);
        }
    }
    return options;
}

// The core resolves paths in its own working folder: always send absolute ones.
Result<std::string> absoluteFolder(const std::string& folder)
{
    auto wide = utf8To16(folder);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(*wide, ec);
    if (ec)
    {
        return fail(ErrorCode::InvalidArgument, std::format("bad path '{}'", folder));
    }
    return utf16To8(absolute.native());
}

void printDiagnostics(const Json& diagnostics)
{
    for (const auto& d : diagnostics)
    {
        const auto line = json::intOr(d, "line", 0);
        const std::string where = line > 0 ? std::format("rice.toml:{}:{}", line, json::intOr(d, "column", 0))
                                           : std::string("rice.toml");
        const std::string key = json::stringOr(d, "key", "");
        err(std::format("{}: {}{}: {}", where, json::stringOr(d, "severity", "error"),
                        key.empty() ? "" : std::format(" [{}]", key), json::stringOr(d, "message", "")));
    }
}

void printReport(const Json& report, std::string_view done)
{
    std::string modules;
    for (const auto& module : json::memberOr(report, "modules", Json::array()))
    {
        modules += (modules.empty() ? "" : ", ") + (module.is_string() ? module.get<std::string>() : "?");
    }
    const auto historyId = json::intOr(report, "historyId", 0);
    if (historyId == 0)
    {
        out("Nothing to change.");
    }
    else
    {
        out(std::format("{}: {} (history #{})", done, modules, historyId));
    }
    for (const auto& skipped : json::memberOr(report, "skipped", Json::array()))
    {
        out(std::format("  kept {}.{}: {} (use --force to restore it anyway)",
                        json::stringOr(skipped, "module", ""), json::stringOr(skipped, "field", ""),
                        json::stringOr(skipped, "reason", "")));
    }
    for (const auto& warning : json::memberOr(report, "warnings", Json::array()))
    {
        out("  note: " + (warning.is_string() ? warning.get<std::string>() : std::string("?")));
    }
}

int failure(const Error& error)
{
    if (error.code() == ErrorCode::Busy)
    {
        err("rmw: busy: " + error.message() + " (or pass --wait)");
        return Busy;
    }
    err("rmw: " + error.message());
    return Failed;
}

// Runs an operation, retrying while the core is busy when --wait is given.
Result<Json> operation(CoreClient& client, std::string_view method, const Json& params, bool wait)
{
    const auto deadline = std::chrono::steady_clock::now() + 5min;
    while (true)
    {
        auto result = client.call(method, params, 5min);
        if (result || result.error().code() != ErrorCode::Busy || !wait ||
            std::chrono::steady_clock::now() > deadline)
        {
            return result;
        }
        std::this_thread::sleep_for(1s);
    }
}

int run(std::span<const std::string> args)
{
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help")
    {
        out(kUsage);
        return Ok;
    }
    if (args[0] == "--version")
    {
        out(std::format("{} {}", kProductName, kVersion));
        return Ok;
    }
    auto options = parseOptions(args.subspan(1));
    if (!options)
    {
        err("rmw: " + options.error().message());
        return Usage;
    }
    const std::string& command = args[0];
    const auto needsFolder = command == "apply" || command == "validate" || command == "export";
    if (needsFolder && options->positional.size() != 1)
    {
        err(std::format("rmw: '{}' needs exactly one folder\n\n{}", command, kUsage));
        return Usage;
    }

    CoreClient client(options->dataDirectory);
    if (auto connected = client.connect(); !connected)
    {
        return failure(connected.error());
    }

    if (command == "validate" || command == "apply")
    {
        auto folder = absoluteFolder(options->positional[0]);
        if (!folder)
        {
            err("rmw: " + folder.error().message());
            return Usage;
        }
        auto checked = client.call("rice.validate", Json{{"path", *folder}}, 30s);
        if (!checked)
        {
            return failure(checked.error());
        }
        printDiagnostics(json::memberOr(*checked, "diagnostics", Json::array()));
        if (!json::boolOr(*checked, "ok", false))
        {
            return InvalidRice;
        }
        if (command == "validate")
        {
            out(std::format("\"{}\" is valid.", json::stringOr(*checked, "name", "")));
            return Ok;
        }
        auto applied = operation(client, "rice.apply", Json{{"path", *folder}}, options->wait);
        if (!applied)
        {
            return failure(applied.error());
        }
        printReport(*applied, std::format("Applied \"{}\"", json::stringOr(*applied, "name", "")));
        return Ok;
    }
    if (command == "restore")
    {
        if (options->original == options->snapshot.has_value())
        {
            err("rmw: restore needs exactly one of --original or --snapshot <id>");
            return Usage;
        }
        auto restored =
            options->original
                ? operation(client, "restore.original", Json{{"force", options->force}}, options->wait)
                : operation(client, "snapshot.restore",
                            Json{{"id", *options->snapshot}, {"force", options->force}}, options->wait);
        if (!restored)
        {
            return failure(restored.error());
        }
        printReport(*restored, "Restored");
        return Ok;
    }
    if (command == "status")
    {
        auto status = client.call("status.get", Json::object(), 30s);
        if (!status)
        {
            return failure(status.error());
        }
        out(std::format("Ricing Manager {} - data in {}", json::stringOr(*status, "version", "?"),
                        json::stringOr(*status, "dataDirectory", "?")));
        const auto captured = json::intOr(*status, "originalCapturedAt", 0);
        out(captured > 0 ? std::format("Original Windows captured on {}", timeText(captured))
                         : std::string("Nothing applied yet."));
        std::string managed;
        for (const auto& module : json::memberOr(*status, "managedModules", Json::array()))
        {
            managed += (managed.empty() ? "" : ", ") + (module.is_string() ? module.get<std::string>() : "?");
        }
        out("Managed now: " + (managed.empty() ? std::string("nothing") : managed));
        if (json::boolOr(*status, "needsRestore", false))
        {
            out("WARNING: an interrupted operation could not be undone yet; it is retried on every start.");
        }
        if (json::boolOr(*status, "busy", false))
        {
            out("An operation is running right now.");
        }
        std::string others;
        for (const auto& tool : json::memberOr(*status, "otherTools", Json::array()))
        {
            others += (others.empty() ? "" : ", ") + (tool.is_string() ? tool.get<std::string>() : "?");
        }
        if (!others.empty())
        {
            out("Other customization tools found: " + others + " (they may change the same settings)");
        }
        return Ok;
    }
    if (command == "history")
    {
        auto history = client.call("snapshot.list", Json::object(), 30s);
        if (!history)
        {
            return failure(history.error());
        }
        if (history->empty())
        {
            out("No operations yet.");
        }
        for (const auto& entry : *history)
        {
            out(std::format("#{:<4} {}  {}", json::intOr(entry, "id", 0),
                            timeText(json::intOr(entry, "appliedAt", 0)),
                            json::stringOr(entry, "label", "")));
        }
        return Ok;
    }
    if (command == "monitors")
    {
        auto monitors = client.call("monitors.list", Json::object(), 30s);
        if (!monitors)
        {
            return failure(monitors.error());
        }
        for (const auto& m : *monitors)
        {
            out(std::format("{}{}  {}x{} at ({},{})  key: {}", json::stringOr(m, "name", "?"),
                            json::boolOr(m, "primary", false) ? " (primary)" : "", json::intOr(m, "width", 0),
                            json::intOr(m, "height", 0), json::intOr(m, "left", 0), json::intOr(m, "top", 0),
                            json::stringOr(m, "key", "")));
        }
        return Ok;
    }
    if (command == "export")
    {
        auto folder = absoluteFolder(options->positional[0]);
        if (!folder)
        {
            err("rmw: " + folder.error().message());
            return Usage;
        }
        Json params{{"path", *folder}};
        if (!options->name.empty())
        {
            params["name"] = options->name;
        }
        auto exported = operation(client, "rice.export", params, options->wait);
        if (!exported)
        {
            return failure(exported.error());
        }
        out(std::format("Exported your current look to {} ({} files).", *folder,
                        json::intOr(*exported, "files", 0)));
        for (const auto& note : json::memberOr(*exported, "notes", Json::array()))
        {
            out("  note: " + (note.is_string() ? note.get<std::string>() : std::string("?")));
        }
        printDiagnostics(json::memberOr(*exported, "diagnostics", Json::array()));
        return json::boolOr(*exported, "valid", false) ? Ok : Failed;
    }
    if (command == "inspect")
    {
        if (options->positional.size() != 1)
        {
            err("rmw: inspect needs a module name (rmw inspect wallpaper)");
            return Usage;
        }
        auto state = client.call("module.inspect", Json{{"module", options->positional[0]}}, 60s);
        if (!state)
        {
            return failure(state.error());
        }
        out(json::dumpPretty(*state));
        return Ok;
    }
    if (command == "action")
    {
        if (options->positional.size() != 1)
        {
            err("rmw: action needs a name (rmw action appearance.toggle)");
            return Usage;
        }
        Json params{{"name", options->positional[0]}, {"params", Json::object()}};
        if (!options->path.empty())
        {
            auto folder = absoluteFolder(options->path);
            if (!folder)
            {
                err("rmw: " + folder.error().message());
                return Usage;
            }
            params["params"]["path"] = *folder;
        }
        auto done = operation(client, "action.run", params, options->wait);
        if (!done)
        {
            return failure(done.error());
        }
        printReport(*done, std::format("Done ({})", options->positional[0]));
        return Ok;
    }
    if (command == "shutdown")
    {
        auto stopped = client.call("shutdown", Json::object(), 10s);
        if (!stopped)
        {
            return failure(stopped.error());
        }
        out("The core is stopping.");
        return Ok;
    }

    err(std::format("rmw: unknown command '{}'\n\n{}", command, kUsage));
    return Usage;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    // Process boundary: nothing may escape as an exception (docs/08 §3 rule 5).
    try
    {
        if (auto hardened = rmw::platform::hardenProcess(); !hardened)
        {
            rmw::platform::writeLine(rmw::platform::Stream::Err,
                                     "rmw: warning: " + hardened.error().toString());
        }

        rmw::log::instance().addSink(rmw::platform::makeConsoleSink());
        rmw::log::instance().setLevel(rmw::log::Level::Warn);

        std::vector<std::string> args;
        args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
        for (int i = 1; i < argc; ++i)
        {
            auto arg = rmw::utf16To8(argv[i]);
            if (!arg)
            {
                rmw::log::error("invalid command-line argument #{}: {}", i, arg.error().toString());
                return Usage;
            }
            args.push_back(std::move(*arg));
        }
        return run(args);
    }
    catch (const std::exception& e)
    {
        rmw::platform::writeLine(rmw::platform::Stream::Err, std::string("rmw: fatal: ") + e.what());
    }
    catch (...)
    {
        rmw::platform::writeLine(rmw::platform::Stream::Err, "rmw: fatal: unknown exception");
    }
    return Failed;
}
