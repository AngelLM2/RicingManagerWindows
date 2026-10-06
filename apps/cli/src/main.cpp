// rmw.exe — command-line client of rmw-core (docs/04 M1). English on purpose for v1.0.

#include "PluginTools.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/client/CoreClient.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/package/Package.hpp"
#include "rmw/platform/Console.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"
#include "rmw/platform/ProcessHardening.hpp"
#include "rmw/plugins/HostPlan.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
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
  pack <folder> [--out <file>]          Make the .rice to share the rice in <folder>
  import <file.rice>                    Add a shared rice to your library (every file is
                                        checked first, in a process of its own)
  monitors                              Connected monitors and their keys (for edid:<key>)
  apps                                  Installed apps and their ids (for launch.app)
  inspect <module>                      What Windows has now for a module (appearance,
                                        taskbar, cursors, wallpaper), as JSON
  action <name> [--path <folder>] [--app <id>]
                                        Run an action: rice.apply, rice.undo, restore.original,
                                        appearance.toggle, appearance.dark, appearance.light,
                                        bar.on, bar.off, bar.toggle, wallpaper.next, launch.app, or a
                                        plugin's own: <plugin id>:<action>
  settings reload                       Re-read config\settings.toml (hotkeys, rules, tray)
  autostart on|off|status               Start with Windows (a per-user Run entry; off until
                                        you turn it on)
  shutdown [--wait]                     Stop the background core (--wait: until it and its
                                        helpers have ended)
  plugin list                           Installed plugins, what they ask for, whether they run
  plugin new <kind> <folder> [--id <id>]
                                        Start a plugin from a template: widget, action, rule,
                                        tweak or skin
  plugin validate <folder>              Run the install checks on a plugin folder
  plugin dev <folder> [--yes] [--no-log]
                                        Run a plugin from its folder: it reloads when you save,
                                        and its log shows here (Ctrl+C stops showing it)
  plugin dev <id> --stop                Stop running it from its folder
  plugin pack <folder> [--out <file>]   Make the .rmwp to share it
  plugin install <folder|file.rmwp> [--yes]
                                        Install a plugin (shows what it asks for)
  plugin approve <id> [--yes]           Approve what an installed plugin asks for
  plugin enable|disable <id>            Run an action or rule plugin without a rice, or stop it
  plugin remove <id>                    Uninstall a plugin and its data
  plugin status                         What the plugin host runs now
  --version, --help

Restores skip settings you changed yourself after applying; --force restores them too.

Exit codes: 0 ok, 1 failed, 2 usage, 3 busy, 4 the rice (or plugin) has errors.)";

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

using client::CoreClient;

// ---- commands -------------------------------------------------------------------------------------

struct Options
{
    std::vector<std::string> positional;
    bool wait = false;
    bool force = false;
    bool original = false;
    bool yes = false;
    bool stop = false;
    bool noLog = false;
    std::string out;
    std::string id;
    std::optional<std::int64_t> snapshot;
    std::string path;
    std::string name;
    std::string app;
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
        else if (arg == "--yes")
        {
            options.yes = true;
        }
        else if (arg == "--stop")
        {
            options.stop = true;
        }
        else if (arg == "--no-log")
        {
            options.noLog = true;
        }
        else if (arg == "--snapshot" || arg == "--path" || arg == "--data-dir" || arg == "--name" ||
                 arg == "--out" || arg == "--id" || arg == "--app")
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
            else if (arg == "--out")
            {
                options.out = std::move(*text);
            }
            else if (arg == "--id")
            {
                options.id = std::move(*text);
            }
            else if (arg == "--app")
            {
                options.app = std::move(*text);
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
    const bool look = json::boolOr(report, "windowLook", false);
    if (historyId == 0)
    {
        // A rice of bars (or [windows]) only: Windows' own settings stay, Ricing Manager's come.
        const auto bars = json::intOr(report, "bars", 0);
        std::string ours = bars > 0 ? std::format("Ricing Manager's bar ({})", bars) : std::string();
        if (look)
        {
            ours += (ours.empty() ? "" : ", ") + std::string("the look of other apps' windows");
        }
        if (json::boolOr(report, "machineIcons", false))
        {
            ours += (ours.empty() ? "" : ", ") + std::string("the folder and drive icons of the whole PC");
        }
        out(!ours.empty() ? std::format("{}: {}; nothing of Windows changed", done, ours)
                          : std::string("Nothing to change."));
    }
    else
    {
        out(std::format("{}: {}{} (history #{})", done, modules, look ? ", other apps' windows" : "", historyId));
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

// ---- plugins -------------------------------------------------------------------------------------

// y/N on the console; a script (no console) must pass --yes.
bool confirm(std::string_view question)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || GetConsoleMode(input, &mode) == FALSE)
    {
        err("rmw: not asking without a console; pass --yes to agree");
        return false;
    }
    out(std::format("{} [y/N]", question));
    std::array<wchar_t, 16> buffer{};
    DWORD read = 0;
    if (ReadConsoleW(input, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr) == FALSE ||
        read == 0)
    {
        return false;
    }
    return buffer[0] == L'y' || buffer[0] == L'Y';
}

void printPermissions(const domain::PluginPermissions& permissions)
{
    const auto lines = cli::describePermissions(permissions);
    if (lines.empty())
    {
        out("It asks for no permissions.");
        return;
    }
    out("It asks to:");
    for (const auto& line : lines)
    {
        out("  " + line);
    }
    if (permissions.hasSensitive())
    {
        out("(!) touches your privacy or reaches outside this PC.");
    }
}

// Prints what the checks found; false when there are errors.
bool printCheck(const cli::PluginCheck& check)
{
    for (const auto& warning : check.warnings)
    {
        err("warning: " + warning);
    }
    for (const auto& error : check.errors)
    {
        err("error: " + error);
    }
    return check.errors.empty();
}

// Shows the plugin's lines of the host's and the bar's logs as they come, until Ctrl+C.
void followLogs(const std::filesystem::path& dataDirectory, const std::string& id)
{
    struct Tail
    {
        std::filesystem::path file;
        std::uint64_t at = 0;
        std::string partial;
    };
    std::array<Tail, 2> tails{Tail{dataDirectory / L"logs" / L"plugins.log", 0, {}},
                              Tail{dataDirectory / L"logs" / L"bar.log", 0, {}}};
    for (auto& tail : tails)
    {
        std::error_code ec;
        tail.at = std::filesystem::file_size(tail.file, ec);
    }
    while (true)
    {
        for (auto& tail : tails)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(tail.file, ec);
            if (ec)
            {
                continue;
            }
            if (size < tail.at)
            {
                tail.at = 0; // rotated
            }
            if (size == tail.at)
            {
                continue;
            }
            HANDLE file = CreateFileW(tail.file.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            LARGE_INTEGER offset{};
            offset.QuadPart = static_cast<LONGLONG>(tail.at);
            SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
            std::string chunk(static_cast<std::size_t>(std::min<std::uint64_t>(size - tail.at, 1u << 20)),
                              '\0');
            DWORD read = 0;
            ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
            CloseHandle(file);
            tail.at += read;
            tail.partial.append(chunk.data(), read);
            for (std::size_t end = tail.partial.find('\n'); end != std::string::npos;
                 end = tail.partial.find('\n'))
            {
                std::string line = tail.partial.substr(0, end);
                tail.partial.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (line.find(id) != std::string::npos)
                {
                    out(line);
                }
            }
        }
        std::this_thread::sleep_for(250ms);
    }
}

// Unpacking a package from anyone takes the core's import worker (docs/06 §4); a 1 GB one, a while.
constexpr auto kImportTimeout = std::chrono::minutes(11);

// A .rmwp: the core unpacks and checks it, installs it unapproved, and it is approved here once
// what it asks for was shown.
int installPackage(CoreClient& client, const std::string& file, const Options& options)
{
    auto entry = client.call("plugin.install", Json{{"path", file}, {"approve", false}}, kImportTimeout);
    if (!entry)
    {
        return failure(entry.error());
    }
    const std::string id = json::stringOr(*entry, "id", "?");
    out(std::format("{} {} — {}", json::stringOr(*entry, "name", id), json::stringOr(*entry, "version", "?"),
                    id));
    if (auto permissions =
            plugins::permissionsFromJson(json::memberOr(*entry, "permissions", Json::object())))
    {
        printPermissions(*permissions);
    }
    if (json::boolOr(*entry, "needsApproval", false))
    {
        if (!options.yes && !confirm("Allow it?"))
        {
            out("Not approved: it is installed but its script will not run until `rmw plugin approve " + id +
                "`.");
            return Ok;
        }
        if (auto approved = client.call("plugin.approve", Json{{"id", id}}, 60s); !approved)
        {
            return failure(approved.error());
        }
    }
    out(std::format("Installed {}.", id));
    return Ok;
}

// The .rice of a rice folder, made here (no core): what an import would refuse is refused now.
// rmw validate: the same check the core makes (the parser), here: no core needs to run, nothing of
// Windows is touched.
int validateRice(const std::string& where)
{
    auto folder = absoluteFolder(where);
    if (!folder)
    {
        err("rmw: " + folder.error().message());
        return Usage;
    }
    const std::filesystem::path root(utf8To16Lossy(*folder));
    auto bytes = platform::readFileLimited(root / L"rice.toml", limits::kManifestMaxBytes);
    if (!bytes)
    {
        return failure(std::move(bytes.error()).withContext("read rice.toml"));
    }
    domain::Diagnostics diagnostics;
    const auto rice = domain::parseRice(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), diagnostics);
    for (const auto& d : diagnostics.all())
    {
        const std::string place = d.line > 0 ? std::format("rice.toml:{}:{}", d.line, d.column) : "rice.toml";
        err(std::format("{}: {}{}: {}", place, d.severity == domain::Severity::Error ? "error" : "warning",
                        d.key.empty() ? "" : std::format(" [{}]", d.key), d.message));
    }
    if (!rice)
    {
        return InvalidRice;
    }
    out(std::format("\"{}\" is valid.", rice->meta.name));
    return Ok;
}

int packRice(const std::string& where, const std::string& target)
{
    auto folder = absoluteFolder(where);
    if (!folder)
    {
        err("rmw: " + folder.error().message());
        return Usage;
    }
    const std::filesystem::path root(utf8To16Lossy(*folder));
    auto bytes = platform::readFileLimited(root / L"rice.toml", limits::kManifestMaxBytes);
    if (!bytes)
    {
        return failure(std::move(bytes.error()).withContext("read rice.toml"));
    }
    domain::Diagnostics diagnostics;
    if (auto rice = domain::parseRice(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), diagnostics);
        !rice)
    {
        err("rmw: rice.toml: " + rice.error().message());
        return InvalidRice;
    }
    const std::filesystem::path archive = target.empty()
                                              ? root.parent_path() / (root.filename().native() + L".rice")
                                              : std::filesystem::absolute(utf8To16Lossy(target));
    auto packed = package::pack(root, package::Kind::Rice, archive);
    if (!packed)
    {
        return failure(packed.error());
    }
    out(std::format("Packed {} ({} KB)", utf16To8Lossy(archive.native()), (*packed + 1023) / 1024));
    return Ok;
}

int pluginCommand(const Options& options)
{
    const auto& args = options.positional;
    const std::string sub = args.empty() ? std::string() : args[0];
    const auto folderAt = [&](std::size_t index) -> Result<std::filesystem::path> {
        if (index >= args.size())
        {
            return fail(ErrorCode::InvalidArgument, std::format("'plugin {}' needs a folder", sub));
        }
        auto folder = absoluteFolder(args[index]);
        if (!folder)
        {
            return std::unexpected(std::move(folder.error()));
        }
        return std::filesystem::path(utf8To16Lossy(*folder));
    };

    // Local: no core needed.
    if (sub == "validate" || sub == "pack")
    {
        auto folder = folderAt(1);
        if (!folder)
        {
            err("rmw: " + folder.error().message());
            return Usage;
        }
        const auto check = cli::checkPlugin(*folder);
        if (!printCheck(check))
        {
            return InvalidRice;
        }
        const auto& manifest = *check.manifest;
        if (sub == "validate")
        {
            std::string kinds;
            for (const auto kind : manifest.kinds)
            {
                kinds += (kinds.empty() ? "" : ", ") + std::string(domain::toString(kind));
            }
            out(std::format("{} {} ({}): OK", manifest.id, manifest.version.toString(), kinds));
            printPermissions(manifest.permissions);
            return Ok;
        }
        const std::filesystem::path target =
            options.out.empty()
                ? folder->parent_path() /
                      utf8To16Lossy(std::format("{}-{}.rmwp", manifest.id, manifest.version.toString()))
                : std::filesystem::absolute(utf8To16Lossy(options.out));
        auto packed = package::pack(*folder, package::Kind::Plugin, target);
        if (!packed)
        {
            return failure(packed.error());
        }
        out(std::format("Packed {} ({} KB)", utf16To8Lossy(target.native()), (*packed + 1023) / 1024));
        return Ok;
    }
    if (sub == "new")
    {
        if (args.size() != 3)
        {
            err("rmw: usage: rmw plugin new <widget|action|rule|tweak|skin> <folder> [--id <id>]");
            return Usage;
        }
        auto folder = folderAt(2);
        if (!folder)
        {
            err("rmw: " + folder.error().message());
            return Usage;
        }
        if (auto created = cli::newPlugin(args[1], *folder, options.id); !created)
        {
            return failure(created.error());
        }
        out(std::format("Created {}. Try it: rmw plugin dev \"{}\"", utf16To8Lossy(folder->native()),
                        utf16To8Lossy(folder->native())));
        return Ok;
    }

    constexpr std::array<std::string_view, 9> kWithCore = {
        "list", "status", "approve", "enable", "disable", "remove", "install", "dev", "help"};
    if (std::ranges::find(kWithCore, sub) == kWithCore.end() || sub == "help")
    {
        err(std::format("rmw: unknown plugin command '{}'\n\n{}", sub, kUsage));
        return Usage;
    }
    CoreClient client(options.dataDirectory);
    if (auto connected = client.connect(); !connected)
    {
        return failure(connected.error());
    }
    if (sub == "list")
    {
        auto list = client.call("plugin.list", Json::object(), 30s);
        if (!list)
        {
            return failure(list.error());
        }
        if (list->empty())
        {
            out("No plugins. Install one with: rmw plugin install <folder>");
            return Ok;
        }
        for (const auto& plugin : *list)
        {
            std::string state = json::boolOr(plugin, "dev", false) ? "dev" : "installed";
            if (!json::stringOr(plugin, "problem", "").empty())
            {
                state = "broken: " + json::stringOr(plugin, "problem", "");
            }
            else if (!json::stringOr(plugin, "disabled", "").empty())
            {
                state = "stopped: " + json::stringOr(plugin, "disabled", "");
            }
            else if (json::boolOr(plugin, "needsApproval", false))
            {
                state = "waits for approval";
            }
            const Json host = json::memberOr(plugin, "host", Json::object());
            if (!json::stringOr(host, "error", "").empty())
            {
                state += ", error: " + json::stringOr(host, "error", "");
            }
            else if (host.contains("memory"))
            {
                state += std::format(", running ({} KB)", json::intOr(host, "memory", 0) / 1024);
            }
            std::string kinds;
            for (const auto& kind : json::memberOr(plugin, "kinds", Json::array()))
            {
                kinds += (kinds.empty() ? "" : ",") + kind.get<std::string>();
            }
            out(std::format("{} {} [{}]{} - {}", json::stringOr(plugin, "id", "?"),
                            json::stringOr(plugin, "version", "?"), kinds,
                            json::boolOr(plugin, "enabled", false) ? " on" : "", state));
            for (const auto& action : json::memberOr(plugin, "actions", Json::array()))
            {
                out(std::format("    action {}: {}", json::stringOr(action, "name", "?"),
                                json::stringOr(action, "label", "")));
            }
        }
        return Ok;
    }
    if (sub == "status")
    {
        auto status = client.call("plugin.status", Json::object(), 30s);
        if (!status)
        {
            return failure(status.error());
        }
        out(json::dumpPretty(*status));
        return Ok;
    }
    if (sub == "install" || sub == "dev")
    {
        if (sub == "dev" && options.stop)
        {
            if (args.size() != 2)
            {
                err("rmw: usage: rmw plugin dev <id> --stop");
                return Usage;
            }
            auto stopped = client.call("plugin.dev", Json{{"id", args[1]}, {"stop", true}}, 30s);
            if (!stopped)
            {
                return failure(stopped.error());
            }
            out(std::format("{} no longer runs from its folder.", args[1]));
            return Ok;
        }
        if (sub == "install" && args.size() == 2)
        {
            auto file = absoluteFolder(args[1]);
            std::error_code ec;
            if (file && std::filesystem::is_regular_file(utf8To16Lossy(*file), ec))
            {
                return installPackage(client, *file, options);
            }
        }
        auto folder = folderAt(1);
        if (!folder)
        {
            err("rmw: " + folder.error().message());
            return Usage;
        }
        const auto check = cli::checkPlugin(*folder);
        if (!printCheck(check))
        {
            return InvalidRice;
        }
        const auto& manifest = *check.manifest;
        out(std::format("{} {} — {}", manifest.name, manifest.version.toString(), manifest.id));
        printPermissions(manifest.permissions);
        const auto& asked = manifest.permissions;
        const bool asks =
            !asked.providers.empty() || !asked.actions.empty() || !asked.network.empty() || asked.storage;
        bool approve = options.yes;
        if (asks && !approve)
        {
            approve = confirm("Allow it?");
            if (!approve)
            {
                out("Not approved: it is installed but its script will not run until `rmw plugin approve " +
                    manifest.id + "`.");
            }
        }
        const std::string method = sub == "install" ? "plugin.install" : "plugin.dev";
        auto entry =
            client.call(method, Json{{"path", utf16To8Lossy(folder->native())}, {"approve", approve}}, 60s);
        if (!entry)
        {
            return failure(entry.error());
        }
        if (sub == "install")
        {
            out(std::format("Installed {}.", manifest.id));
            return Ok;
        }
        out(std::format("{} runs from {}: save a file and it reloads.", manifest.id,
                        utf16To8Lossy(folder->native())));
        if (options.noLog)
        {
            return Ok;
        }
        out("Its log follows (Ctrl+C to stop showing it; `rmw plugin dev " + manifest.id +
            " --stop` to stop it):");
        auto data = options.dataDirectory.empty() ? platform::defaultDataDirectory()
                                                  : Result<std::filesystem::path>(options.dataDirectory);
        if (data)
        {
            followLogs(*data, manifest.id);
        }
        return Ok;
    }
    if (args.size() != 2)
    {
        err(std::format("rmw: usage: rmw plugin {} <id>", sub));
        return Usage;
    }
    const std::string& id = args[1];
    if (sub == "approve")
    {
        auto list = client.call("plugin.list", Json::object(), 30s);
        if (!list)
        {
            return failure(list.error());
        }
        const auto found =
            std::ranges::find_if(*list, [&](const Json& p) { return json::stringOr(p, "id", "") == id; });
        if (found == list->end())
        {
            err(std::format("rmw: plugin '{}' is not installed", id));
            return Failed;
        }
        auto permissions =
            plugins::permissionsFromJson(json::memberOr(*found, "permissions", Json::object()));
        if (permissions)
        {
            printPermissions(*permissions);
        }
        if (!options.yes && !confirm("Allow it?"))
        {
            out("Nothing changed.");
            return Ok;
        }
    }
    const std::string method = sub == "approve"   ? "plugin.approve"
                               : sub == "enable"  ? "plugin.enable"
                               : sub == "disable" ? "plugin.disable"
                                                  : "plugin.remove";
    auto done = client.call(method, Json{{"id", id}}, 60s);
    if (!done)
    {
        return failure(done.error());
    }
    out(sub == "approve"   ? std::format("{} approved.", id)
        : sub == "enable"  ? std::format("{} is on.", id)
        : sub == "disable" ? std::format("{} is off.", id)
                           : std::format("{} removed.", id));
    return Ok;
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
    if (command == "plugin")
    {
        return pluginCommand(*options);
    }
    constexpr std::array<std::string_view, 15> kCommands = {
        "apps",     "apply",   "validate", "status",   "history",   "restore",  "export", "import",
        "monitors", "inspect", "action",   "settings", "autostart", "shutdown", "pack",
    };
    if (std::ranges::find(kCommands, command) == kCommands.end())
    {
        // Before connecting: a typo must not start the core.
        err(std::format("rmw: unknown command '{}'\n\n{}", command, kUsage));
        return Usage;
    }
    const auto needsFolder = command == "apply" || command == "validate" || command == "export" ||
                             command == "pack" || command == "import";
    if (needsFolder && options->positional.size() != 1)
    {
        err(std::format("rmw: '{}' needs exactly one {}\n\n{}", command,
                        command == "import" ? "file" : "folder", kUsage));
        return Usage;
    }
    if (command == "pack")
    {
        return packRice(options->positional[0], options->out);
    }
    if (command == "validate")
    {
        return validateRice(options->positional[0]);
    }

    CoreClient client(options->dataDirectory);
    if (auto connected = client.connect(); !connected)
    {
        return failure(connected.error());
    }

    if (command == "apply")
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
        const Json safeMode = json::memberOr(*status, "safeMode", Json());
        if (safeMode.is_string())
        {
            out(std::format("SAFE MODE ({}): nothing runs automatically; restore still works.",
                            safeMode.get<std::string>() == "crashes" ? "the last starts crashed"
                                                                     : "requested"));
        }
        const Json schedule = json::memberOr(*status, "schedule", Json());
        if (schedule.is_object())
        {
            out(std::format("Appearance schedule: light at {}, dark at {}",
                            json::stringOr(schedule, "lightAt", "?"),
                            json::stringOr(schedule, "darkAt", "?")));
        }
        const Json barStatus = json::memberOr(*status, "bar", Json::object());
        if (json::boolOr(barStatus, "configured", false))
        {
            std::string state = json::boolOr(barStatus, "off", false) ? "off (rmw action bar.on)"
                                : json::boolOr(barStatus, "disabled", false)
                                    ? "stopped after failing too often"
                                : json::boolOr(barStatus, "running", false)
                                    ? std::format("running (pid {})", json::intOr(barStatus, "pid", 0))
                                    : "starting";
            if (const auto restarts = json::intOr(barStatus, "restarts", 0); restarts > 0)
            {
                state += std::format(", restarted {} time(s)", restarts);
            }
            out("Bar: " + state + " - Ctrl+Alt+Win+T brings Windows' taskbar back");
        }
        const Json videoStatus = json::memberOr(*status, "video", Json::object());
        if (json::boolOr(videoStatus, "configured", false))
        {
            std::string state = json::boolOr(videoStatus, "disabled", false)
                                    ? "stopped after failing too often"
                                : json::boolOr(videoStatus, "running", false)
                                    ? std::format("running (pid {})", json::intOr(videoStatus, "pid", 0))
                                    : "starting";
            if (const auto restarts = json::intOr(videoStatus, "restarts", 0); restarts > 0)
            {
                state += std::format(", restarted {} time(s)", restarts);
            }
            if (const auto adapter = json::stringOr(videoStatus, "adapter", ""); !adapter.empty())
            {
                state += ", on " + adapter;
            }
            out("Live wallpaper: " + state);
            if (const auto error = json::stringOr(videoStatus, "error", ""); !error.empty())
            {
                out("  " + error + " (the rice's fallback image shows)");
            }
            for (const auto& monitor : json::memberOr(videoStatus, "monitors", Json::array()))
            {
                std::string line = json::stringOr(monitor, "state", "?");
                if (const auto paused = json::stringOr(monitor, "paused", ""); !paused.empty())
                {
                    line += " (" + paused + ")";
                }
                if (const auto video = json::stringOr(monitor, "video", ""); !video.empty())
                {
                    line += ": " + video;
                }
                if (const auto error = json::stringOr(monitor, "error", ""); !error.empty())
                {
                    line += ": " + error;
                }
                out(std::format("  {}: {}", json::stringOr(monitor, "monitor", "?"), line));
            }
        }
        const Json tilerStatus = json::memberOr(*status, "tiler", Json::object());
        if (json::boolOr(tilerStatus, "configured", false))
        {
            std::string state = json::boolOr(tilerStatus, "disabled", false)
                                    ? "stopped after failing too often (the windows went back)"
                                : json::boolOr(tilerStatus, "running", false)
                                    ? std::format("running (pid {})", json::intOr(tilerStatus, "pid", 0))
                                    : "starting";
            if (const auto restarts = json::intOr(tilerStatus, "restarts", 0); restarts > 0)
            {
                state += std::format(", restarted {} time(s)", restarts);
            }
            if (json::boolOr(tilerStatus, "tiling", true))
            {
                out("Tiling: " + state);
            }
            if (json::boolOr(tilerStatus, "windowLook", false))
            {
                out("Other apps' windows: the rice's look (rmw-tiler " + state + ")");
            }
        }
        if (const auto slideshows = json::intOr(*status, "slideshows", 0); slideshows > 0)
        {
            out(std::format("Slideshow turned by Ricing Manager on {} monitor(s)", slideshows));
        }
        const Json resident = json::memberOr(*status, "resident", Json());
        if (resident.is_object())
        {
            const auto hotkeys = json::memberOr(resident, "hotkeys", Json::array());
            out(std::format("{}: {} hotkey(s), {} rule(s)",
                            json::boolOr(resident, "resident", false) ? "Resident (tray icon)"
                                                                      : "Not resident (exits when idle)",
                            hotkeys.size(), json::intOr(resident, "rules", 0)));
            for (const auto& hotkey : hotkeys)
            {
                const std::string error = json::stringOr(hotkey, "error", "");
                out(std::format("  {} -> {}{}", json::stringOr(hotkey, "keys", "?"),
                                json::stringOr(hotkey, "action", "?"),
                                json::boolOr(hotkey, "registered", false)
                                    ? ""
                                    : " (not active" + (error.empty() ? std::string() : ": " + error) + ")"));
            }
            const Json settings = json::memberOr(resident, "settings", Json::object());
            if (!json::boolOr(settings, "valid", true))
            {
                out("settings.toml is not used: " + json::stringOr(settings, "error", "?"));
                printDiagnostics(json::memberOr(settings, "diagnostics", Json::array()));
            }
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
    if (command == "apps")
    {
        auto apps = client.call("apps.list", Json::object(), 60s);
        if (!apps)
        {
            return failure(apps.error());
        }
        for (const auto& app : *apps)
        {
            out(std::format("{}  ->  {}", json::stringOr(app, "name", "?"), json::stringOr(app, "id", "")));
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
    if (command == "import")
    {
        auto file = absoluteFolder(options->positional[0]);
        if (!file)
        {
            err("rmw: " + file.error().message());
            return Usage;
        }
        auto imported = client.call("rice.import", Json{{"path", *file}}, kImportTimeout);
        if (!imported)
        {
            return failure(imported.error());
        }
        const std::string folder = json::stringOr(*imported, "path", "");
        out(std::format("Imported \"{}\" into {} ({} files).", json::stringOr(*imported, "name", ""), folder,
                        json::intOr(*imported, "files", 0)));
        out(std::format("Apply it with: rmw apply \"{}\"", folder));
        return Ok;
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
        if (!options->app.empty())
        {
            params["params"]["app"] = options->app;
        }
        auto done = operation(client, "action.run", params, options->wait);
        if (!done)
        {
            return failure(done.error());
        }
        if (options->positional[0].find(':') != std::string::npos)
        {
            out(std::format("Done ({})", options->positional[0])); // a plugin's action: no report
            return Ok;
        }
        printReport(*done, std::format("Done ({})", options->positional[0]));
        return Ok;
    }
    if (command == "settings")
    {
        if (options->positional.size() != 1 || options->positional[0] != "reload")
        {
            err("rmw: usage: rmw settings reload");
            return Usage;
        }
        auto reloaded = client.call("settings.reload", Json::object(), 30s);
        if (!reloaded)
        {
            return failure(reloaded.error());
        }
        printDiagnostics(json::memberOr(*reloaded, "diagnostics", Json::array()));
        if (!json::boolOr(*reloaded, "valid", false))
        {
            err("rmw: settings.toml was not used (the previous settings stay): " +
                json::stringOr(*reloaded, "error", "?"));
            return Failed;
        }
        out(json::boolOr(*reloaded, "present", false) ? "Settings reloaded."
                                                      : "There is no settings.toml: nothing configured.");
        return Ok;
    }
    if (command == "autostart")
    {
        const std::string choice = options->positional.size() == 1 ? options->positional[0] : std::string();
        if (choice != "on" && choice != "off" && choice != "status")
        {
            err("rmw: usage: rmw autostart on|off|status");
            return Usage;
        }
        auto state = choice == "status"
                         ? client.call("autostart.get", Json::object(), 30s)
                         : client.call("autostart.set", Json{{"enabled", choice == "on"}}, 30s);
        if (!state)
        {
            return failure(state.error());
        }
        if (json::boolOr(*state, "enabled", false))
        {
            out("Ricing Manager starts with Windows.");
        }
        else if (json::boolOr(*state, "registered", false))
        {
            out("A start entry exists but points elsewhere: " + json::stringOr(*state, "command", "?"));
        }
        else
        {
            out("Ricing Manager does not start with Windows.");
        }
        if (json::boolOr(*state, "registered", false) && json::boolOr(*state, "disabledByUser", false))
        {
            out("It is switched off in Task Manager (Startup apps); turn it on there if you want it.");
        }
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
        if (!options->wait)
        {
            return Ok;
        }
        // Until the core and its helpers from this installation are gone (the installer replaces
        // or removes their files next). The bar first hands Windows' taskbar back.
        auto self = platform::currentExecutable();
        if (!self)
        {
            return failure(self.error());
        }
        constexpr std::array<std::wstring_view, 6> kPrograms = {L"rmw-core.exe",        L"rmw-bar.exe",
                                                                L"rmw-wallpaper.exe",   L"rmw-plugin-host.exe",
                                                                L"rmw-settings.exe",    L"rmw-tiler.exe"};
        // The settings window closes like when its X is clicked.
        constexpr std::array<std::wstring_view, 1> kWindows = {L"rmw-settings.exe"};
        auto windows = platform::processesFromFolder(self->parent_path(), kWindows);
        EnumWindows(
            [](HWND window, LPARAM pids) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                // NOLINTNEXTLINE(performance-no-int-to-ptr): EnumWindows hands its context back as an LPARAM
                if (std::ranges::contains(*reinterpret_cast<std::vector<std::uint32_t>*>(pids), pid) &&
                    IsWindowVisible(window) != FALSE)
                {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&windows));
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!platform::processesFromFolder(self->parent_path(), kPrograms).empty())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                err("rmw: the core or one of its helpers is still running after 30 s");
                return Failed;
            }
            std::this_thread::sleep_for(100ms);
        }
        out("Stopped.");
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
