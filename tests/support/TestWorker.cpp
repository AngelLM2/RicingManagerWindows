// rmw_test_worker.exe — a worker that misbehaves on purpose, for the supervisor tests (docs/03 §2.4):
//   rmw_test_worker --mode ok|leak|hang|crash|exit [--heartbeat <handle>] [--core <handle>]
// With --core it exits (code 0) as soon as that process ends, like the bar does.
// It beats on a 100 ms thread timer, like a window thread would; then, after half a second:
//   leak   commits 8 MiB more on every beat
//   hang   blocks its only thread forever
//   crash  dies with an access-violation exit code (without involving error reporting)
//   exit   returns 0

#include "rmw/platform/Supervisor.hpp"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    std::vector<std::wstring_view> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    std::wstring_view mode = L"ok";
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == L"--mode")
        {
            mode = args[i + 1];
        }
    }
    auto heartbeat = rmw::platform::Heartbeat::fromArguments(args);
    if (!heartbeat)
    {
        return 2;
    }

    HANDLE core = nullptr;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == L"--core")
        {
            const auto value = std::wcstoull(std::wstring(args[i + 1]).c_str(), nullptr, 10);
            core = reinterpret_cast<HANDLE>(value); // NOLINT(performance-no-int-to-ptr): inherited
        }
    }

    constexpr std::size_t kLeakStep = std::size_t{8} << 20;
    std::vector<std::unique_ptr<char[]>> leaked; // NOLINT(modernize-avoid-c-arrays): raw blocks
    SetTimer(nullptr, 0, 100, nullptr);
    int ticks = 0;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message != WM_TIMER)
        {
            continue;
        }
        heartbeat->beat();
        if (core != nullptr && WaitForSingleObject(core, 0) == WAIT_OBJECT_0)
        {
            return 0;
        }
        if (++ticks < 5)
        {
            continue;
        }
        if (mode == L"leak")
        {
            auto block =
                std::make_unique_for_overwrite<char[]>(kLeakStep); // NOLINT(modernize-avoid-c-arrays)
            std::memset(block.get(), 1, kLeakStep);                // commit it for real
            leaked.push_back(std::move(block));
        }
        else if (mode == L"hang")
        {
            Sleep(INFINITE);
        }
        else if (mode == L"crash")
        {
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        }
        else if (mode == L"exit")
        {
            return 0;
        }
    }
    return 0;
}
