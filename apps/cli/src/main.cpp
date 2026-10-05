#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/platform/Console.hpp"
#include "rmw/platform/ProcessHardening.hpp"

#include <format>
#include <span>
#include <string>
#include <vector>

namespace
{

constexpr std::string_view kUsage = R"(Usage: rmw <command> [options]

Commands:
  --version    Print the version
  --help       Print this help

More commands arrive with milestone M1 (docs/04-mvp.md).)";

int run(std::span<const std::string> args)
{
    using rmw::platform::Stream;
    using rmw::platform::writeLine;

    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        writeLine(Stream::Out, kUsage);
        return 0;
    }
    if (args[0] == "--version")
    {
        writeLine(Stream::Out, std::format("{} {}", rmw::kProductName, rmw::kVersion));
        return 0;
    }

    writeLine(Stream::Err, std::format("rmw: unknown command '{}'\n\n{}", args[0], kUsage));
    return 2;
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
                return 2;
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
    return 1;
}
