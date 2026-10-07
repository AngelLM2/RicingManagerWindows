#include "Import.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"

#include <chrono>
#include <exception>
#include <format>
#include <new>

namespace rmw::core
{
namespace
{

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Images are read whole (up to 64 MB) before they are written; videos stream.
constexpr std::uint64_t kWorkerMemoryLimit = 384ull << 20;
// A 1 GB package from a slow disk.
constexpr auto kWorkerTimeout = 10min;
constexpr std::uint64_t kReportMaxBytes = 64ull << 10;

std::string text(const fs::path& path)
{
    return utf16To8Lossy(path.native());
}

Result<void> writeReport(const fs::path& report, const json::Json& value)
{
    const std::string body = json::dump(value);
    return platform::writeFileAtomic(report, std::as_bytes(std::span(body)));
}

} // namespace

Result<Unpacked> unpackInWorker(const fs::path& dataDirectory, const fs::path& archive, package::Kind kind)
{
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec))
    {
        return fail(ErrorCode::NotFound, std::format("'{}' is not a file", text(archive)));
    }
    const fs::path imports = dataDirectory / L"imports";
    fs::create_directories(imports, ec);
    auto name = platform::randomHex(8);
    if (!name)
    {
        return std::unexpected(std::move(name.error()));
    }
    const fs::path staging = imports / fs::path(utf8To16Lossy(*name));
    const fs::path report = imports / fs::path(utf8To16Lossy(*name + ".json"));
    if (!fs::create_directory(staging, ec) || ec)
    {
        return fail(ErrorCode::Io, std::format("cannot create {}: {}", text(staging), ec.message()));
    }
    auto cleanup = ScopeExit([&]() noexcept {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    });
    const auto removeReport = ScopeExit([&]() noexcept {
        std::error_code ignored;
        fs::remove(report, ignored);
    });

    auto self = platform::currentExecutable();
    if (!self)
    {
        return std::unexpected(std::move(self.error()));
    }
    platform::ChildOptions options;
    options.memoryLimitBytes = kWorkerMemoryLimit;
    options.noChildProcesses = true;
    using platform::quoteArgument;
    const std::wstring arguments =
        std::format(L"--import-worker --data-dir {} --kind {} --archive {} --staging {} --report {}",
                    quoteArgument(dataDirectory.native()), kind == package::Kind::Rice ? L"rice" : L"plugin",
                    quoteArgument(archive.native()), quoteArgument(staging.native()), quoteArgument(report.native()));
    auto child = platform::ChildProcess::spawn(*self, arguments, options);
    if (!child)
    {
        return std::unexpected(std::move(child.error()).withContext("start the import worker"));
    }
    const auto exitCode = child->wait(kWorkerTimeout);
    if (!exitCode)
    {
        child->terminate();
        return fail(ErrorCode::Timeout, "unpacking took too long");
    }
    auto bytes = platform::readFileLimited(report, kReportMaxBytes);
    if (!bytes)
    {
        // No report: it died (out of memory under the Job's cap, or a crash).
        return fail(ErrorCode::System, std::format("the import worker stopped (exit code {})", *exitCode));
    }
    auto parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!parsed || !parsed->is_object())
    {
        return fail(ErrorCode::System, "the import worker's report is unreadable");
    }
    if (!json::boolOr(*parsed, "ok", false) || *exitCode != 0)
    {
        return fail(ErrorCode::InvalidData, json::stringOr(*parsed, "error", "the package was refused"));
    }
    cleanup.release();
    return Unpacked{staging, static_cast<std::size_t>(json::intOr(*parsed, "files", 0)),
                    static_cast<std::uint64_t>(json::intOr(*parsed, "bytes", 0))};
}

Result<PlacedRice> placeRice(const fs::path& dataDirectory, const Unpacked& unpacked)
{
    auto cleanup = ScopeExit([&]() noexcept {
        std::error_code ignored;
        fs::remove_all(unpacked.folder, ignored);
    });
    auto bytes = platform::readFileLimited(unpacked.folder / L"rice.toml", limits::kManifestMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()).withContext("read rice.toml"));
    }
    domain::Diagnostics diagnostics;
    auto rice = domain::parseRice(
        std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), diagnostics);
    if (!rice)
    {
        return std::unexpected(std::move(rice.error()).withContext("rice.toml"));
    }
    const std::string name = rice->meta.name.empty() ? std::string("Rice") : rice->meta.name;
    const std::string base = domain::safeFileName(name, "Rice");
    const fs::path rices = dataDirectory / L"rices";
    std::error_code ec;
    fs::create_directories(rices, ec);
    for (int attempt = 1; attempt < 1000; ++attempt)
    {
        const std::string folderName = attempt == 1 ? base : std::format("{} {}", base, attempt);
        const fs::path target = rices / fs::path(utf8To16Lossy(folderName));
        if (fs::exists(target, ec))
        {
            continue;
        }
        fs::rename(unpacked.folder, target, ec);
        if (ec)
        {
            return fail(ErrorCode::Io,
                        std::format("cannot put the rice in {}: {}", text(target), ec.message()));
        }
        cleanup.release();
        log::info("import: rice '{}' in {} ({} files)", name, text(target), unpacked.files);
        return PlacedRice{target, name};
    }
    return fail(ErrorCode::AlreadyExists, std::format("too many rices called {}", base));
}

int runImportWorker(const std::wstring& kind, const fs::path& archive, const fs::path& staging,
                    const fs::path& report)
{
    if (kind != L"rice" && kind != L"plugin")
    {
        log::error("import worker: --kind is rice or plugin");
        return 2;
    }
    json::Json result;
    try
    {
        auto imported =
            package::extract(archive, kind == L"rice" ? package::Kind::Rice : package::Kind::Plugin, staging);
        if (imported)
        {
            result = {{"ok", true}, {"files", imported->files}, {"bytes", imported->bytes}};
        }
        else
        {
            log::warn("import worker: {}", imported.error().toString());
            result = {{"ok", false}, {"error", imported.error().message()}};
        }
    }
    catch (const std::bad_alloc&)
    {
        result = {{"ok", false}, {"error", "the package needs more memory than an import may use"}};
    }
    if (auto written = writeReport(report, result); !written)
    {
        log::error("import worker: {}", written.error().toString());
        return 1;
    }
    return json::boolOr(result, "ok", false) ? 0 : 1;
}

} // namespace rmw::core
