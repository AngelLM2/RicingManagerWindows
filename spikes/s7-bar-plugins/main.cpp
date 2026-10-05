// Spike S7 — custom bar + community plugins (docs/04, docs/12 §10).
//
//   spike_s7.exe bar [seconds]   Bar process: builds the bar UI with the Slint *interpreter* from
//                                10 plugin widget sources (validated first), starts the plugin host
//                                and applies the values it streams over a named pipe.
//   spike_s7.exe host <pipe>     Plugin host: 10 sandboxed Luau VMs (memory cap + interrupt), plus
//                                hostile scripts (infinite loop, memory bomb, loadstring).
//
// Measures: bar private bytes and CPU with 10 widgets updating every 250 ms, IPC latency, apply
// cost, Luau memory per VM, and whether the plugin-source validation rejects escaping paths.

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <luau/lua.h>
#include <luau/luacode.h>
#include <luau/lualib.h>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <rpc.h>
#include <slint-interpreter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr int kWidgets = 10;
constexpr size_t kVmMemoryCap = 16u << 20;
constexpr auto kCallbackBudget = std::chrono::milliseconds(50);

double privateMb()
{
    PROCESS_MEMORY_COUNTERS_EX c{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c));
    return c.PrivateUsage / 1048576.0;
}

ULONGLONG cpuTime100ns()
{
    FILETIME a{}, b{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    const auto v = [](FILETIME f) {
        return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return v(k) + v(u);
}

std::int64_t qpcMicros()
{
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1'000'000 / f.QuadPart;
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

// =================================================================================================
// Plugin widget source validation (the interpreter API has no file-loader hook, so plugin .slint
// sources are checked before compiling: imports and image URLs must stay inside the plugin folder).
// =================================================================================================
bool pathEscapes(const std::string& path)
{
    return path.empty() || path.find("..") != std::string::npos || path.find(':') != std::string::npos ||
           path.front() == '/' || path.front() == '\\' || path.find("\\\\") != std::string::npos;
}

std::string validateWidgetSource(const std::string& source)
{
    static const std::regex imageUrl(R"re(@image-url\s*\(\s*"([^"]*)")re");
    static const std::regex importFrom(R"re(\bfrom\s*"([^"]*)")re");
    static const std::regex importFile(R"re(\bimport\s*"([^"]*)")re");
    for (const auto* re : {&imageUrl, &importFrom, &importFile})
    {
        for (std::sregex_iterator it(source.begin(), source.end(), *re), end; it != end; ++it)
        {
            const std::string path = (*it)[1].str();
            if (path == "std-widgets.slint")
            {
                continue;
            }
            if (pathEscapes(path))
            {
                return "path escapes the plugin folder: " + path;
            }
        }
    }
    return {};
}

// =================================================================================================
// Bar process
// =================================================================================================
const char* kWidgetSources[kWidgets] = {
    // 0 workspaces
    R"(export component Widget inherits Rectangle { in property <string> value; border-radius: 6px; background: #7aa2f7; height: 26px; width: 70px; Text { text: root.value; color: #1a1b26; font-weight: 700; horizontal-alignment: center; vertical-alignment: center; } })",
    // 1 window title
    R"(export component Widget inherits Text { in property <string> value; text: root.value; color: #c0caf5; vertical-alignment: center; overflow: elide; width: 260px; })",
    // 2 media
    R"(export component Widget inherits HorizontalLayout { in property <string> value; spacing: 6px; Text { text: "♪"; color: #bb9af7; vertical-alignment: center; } Text { text: root.value; color: #c0caf5; vertical-alignment: center; } })",
    // 3 clock
    R"(export component Widget inherits Text { in property <string> value; text: root.value; color: #c0caf5; font-size: 15px; font-weight: 700; vertical-alignment: center; })",
    // 4 cpu with bar
    R"(export component Widget inherits HorizontalLayout { in property <string> value; spacing: 6px; Text { text: "CPU " + root.value + "%"; color: #9ece6a; vertical-alignment: center; } Rectangle { width: 40px; height: 6px; y: 17px; background: #2a2e42; border-radius: 3px; Rectangle { x: 0; width: parent.width * clamp(root.value.to-float() / 100, 0, 1); background: #9ece6a; border-radius: 3px; } } })",
    // 5 memory
    R"(export component Widget inherits Text { in property <string> value; text: "RAM " + root.value; color: #e0af68; vertical-alignment: center; })",
    // 6 network
    R"(export component Widget inherits Text { in property <string> value; text: "↓ " + root.value; color: #7dcfff; vertical-alignment: center; })",
    // 7 volume
    R"(export component Widget inherits Text { in property <string> value; text: "🔊 " + root.value; color: #c0caf5; vertical-alignment: center; })",
    // 8 battery
    R"(export component Widget inherits Text { in property <string> value; text: "🔋 " + root.value; color: #73daca; vertical-alignment: center; })",
    // 9 notifications pill
    R"(export component Widget inherits Rectangle { in property <string> value; border-radius: 13px; background: #f7768e; height: 26px; width: 34px; Text { text: root.value; color: white; horizontal-alignment: center; vertical-alignment: center; } })",
};

const char* kHostileWidgets[] = {
    R"(import { X } from "C:/Windows/evil.slint"; export component Widget inherits Text { })",
    R"(export component Widget inherits Image { source: @image-url("../../secrets/photo.png"); })",
    R"(export component Widget inherits Image { source: @image-url("\\\\server\\share\\x.png"); })",
    R"(import "D:/fonts/bad.ttf"; export component Widget inherits Text { })",
};

std::string generateBarSource()
{
    std::string imports;
    std::string properties;
    std::string children;
    for (int i = 0; i < kWidgets; ++i)
    {
        imports += std::format("import {{ Widget as W{0} }} from \"p{0}/widget.slint\";\n", i);
        properties += std::format("    in property <string> v{};\n", i);
    }
    const auto widget = [](int i) { return std::format("        W{0} {{ value: root.v{0}; }}\n", i); };
    children += "    HorizontalLayout {\n        padding-left: 10px; padding-right: 10px; spacing: 14px;\n";
    for (int i : {0, 1, 2})
    {
        children += widget(i);
    }
    children += "        Rectangle { horizontal-stretch: 1; }\n";
    children += widget(3);
    children += "        Rectangle { horizontal-stretch: 1; }\n";
    for (int i : {4, 5, 6, 7, 8, 9})
    {
        children += widget(i);
    }
    children += "    }\n";
    return imports +
           "export component Bar inherits Window {\n    no-frame: true;\n    always-on-top: true;\n"
           "    width: 1400px;\n    height: 40px;\n    background: #1a1b26;\n" +
           properties + children + "}\n";
}

int runBar(int seconds)
{
    SetEnvironmentVariableW(L"SLINT_BACKEND", L"winit-software");
    nlohmann::json report;

    // 1. Plugin folders + validation ---------------------------------------------------------
    const fs::path root = fs::temp_directory_path() / "rmw-s7";
    fs::remove_all(root);
    for (int i = 0; i < kWidgets; ++i)
    {
        fs::create_directories(root / std::format("p{}", i));
        std::ofstream(root / std::format("p{}", i) / "widget.slint") << kWidgetSources[i];
    }
    int benignAccepted = 0;
    for (const auto* src : kWidgetSources)
    {
        benignAccepted += validateWidgetSource(src).empty() ? 1 : 0;
    }
    int hostileRejected = 0;
    std::vector<std::string> reasons;
    for (const auto* src : kHostileWidgets)
    {
        const std::string why = validateWidgetSource(src);
        hostileRejected += why.empty() ? 0 : 1;
        reasons.push_back(why);
    }
    report["validation"] = {{"benignAccepted", benignAccepted},
                            {"hostileRejected", hostileRejected},
                            {"hostileTotal", std::size(kHostileWidgets)},
                            {"reasons", reasons}};

    // 2. Compile with the interpreter ----------------------------------------------------------
    const double memBefore = privateMb();
    const auto compileStart = Clock::now();
    slint::interpreter::ComponentCompiler compiler;
    const std::string barSource = generateBarSource();
    const std::string barPath = (root / "bar.slint").string();
    auto definition = compiler.build_from_source(barSource, barPath);
    const double compileMs = std::chrono::duration<double, std::milli>(Clock::now() - compileStart).count();
    if (!definition)
    {
        std::string diag;
        for (const auto& d : compiler.diagnostics())
        {
            diag += std::format("{}:{} {}; ", d.line, d.column, std::string_view(d.message));
        }
        report["error"] = "compile failed: " + diag;
        rmw::platform::writeLine(rmw::platform::Stream::Out, report.dump());
        return 1;
    }
    auto bar = definition->create();
    report["compileMs"] = compileMs;
    report["privateMbBeforeUi"] = memBefore;

    // 3. Plugin host over a named pipe with a random name ----------------------------------------
    UUID uuid{};
    UuidCreate(&uuid);
    RPC_WSTR uuidText = nullptr;
    UuidToStringW(&uuid, &uuidText);
    const std::wstring pipeName = std::wstring(LR"(\\.\pipe\rmw-s7-)") + reinterpret_cast<wchar_t*>(uuidText);
    RpcStringFreeW(&uuidText);
    wil::unique_hfile pipe(
        CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1 << 16, 0, nullptr));
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::format(LR"("{}" host "{}")", self, pipeName);
    STARTUPINFOW si{.cb = sizeof(si)};
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                   &pi);
    wil::unique_handle hostProcess(pi.hProcess);
    wil::unique_handle hostThread(pi.hThread);

    std::mutex statsMutex;
    std::vector<double> latencyMs;
    std::vector<double> applyMs;
    std::atomic<int> batches{0};
    std::atomic<bool> stopReading{false};
    nlohmann::json hostReport;

    std::jthread reader([&] {
        ConnectNamedPipe(pipe.get(), nullptr);
        std::string buffer;
        char chunk[4096];
        DWORD read = 0;
        while (!stopReading && ReadFile(pipe.get(), chunk, sizeof(chunk), &read, nullptr) && read > 0)
        {
            buffer.append(chunk, read);
            for (size_t nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n'))
            {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                auto message = nlohmann::json::parse(line, nullptr, false);
                if (message.is_discarded())
                {
                    continue;
                }
                if (message.contains("report"))
                {
                    const std::lock_guard lock(statsMutex);
                    hostReport = message["report"];
                    continue;
                }
                const std::int64_t sent = message.value("t", std::int64_t{0});
                std::vector<std::string> values = message.value("v", std::vector<std::string>{});
                slint::invoke_from_event_loop([&, sent, values = std::move(values)] {
                    const std::int64_t received = qpcMicros();
                    const auto applyStart = Clock::now();
                    for (int i = 0; i < kWidgets && i < static_cast<int>(values.size()); ++i)
                    {
                        bar->set_property(std::format("v{}", i), slint::SharedString(values[i]));
                    }
                    const std::lock_guard lock(statsMutex);
                    applyMs.push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - applyStart).count());
                    latencyMs.push_back((received - sent) / 1000.0);
                    ++batches;
                });
            }
        }
    });

    // 4. Show the bar as a floating, non-activating tool window ----------------------------------
    bar->show();
    const HWND hwnd = bar->window().win32_hwnd();
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    SetWindowPos(hwnd, HWND_TOPMOST, (screenWidth - 1400) / 2, 8, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

    double memShown = 0;
    ULONGLONG cpuStart = 0;
    Clock::time_point measureStart{};
    int ticks = 0;
    slint::Timer driver(std::chrono::seconds(1), [&] {
        ++ticks;
        if (ticks == 3)
        {
            memShown = privateMb();
            cpuStart = cpuTime100ns();
            measureStart = Clock::now();
        }
        if (ticks >= seconds)
        {
            slint::quit_event_loop();
        }
    });
    slint::run_event_loop();

    const double wall = std::chrono::duration<double>(Clock::now() - measureStart).count();
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const double cpu =
        100.0 * static_cast<double>(cpuTime100ns() - cpuStart) / (wall * 1e7) / info.dwNumberOfProcessors;
    const double memEnd = privateMb();

    // Ask the host for its final report by closing our end; give it a moment.
    stopReading = true;
    CancelIoEx(pipe.get(), nullptr);
    TerminateProcess(hostProcess.get(), 0);

    const std::lock_guard lock(statsMutex);
    report["bar"] = {{"privateMbShown", memShown},
                     {"privateMbEnd", memEnd},
                     {"cpuPercent", cpu},
                     {"batches", batches.load()},
                     {"latencyMsP50", percentile(latencyMs, 0.5)},
                     {"latencyMsP95", percentile(latencyMs, 0.95)},
                     {"applyMsP50", percentile(applyMs, 0.5)},
                     {"applyMsP95", percentile(applyMs, 0.95)}};
    report["host"] = hostReport;
    rmw::platform::writeLine(rmw::platform::Stream::Out, report.dump());
    return 0;
}

// =================================================================================================
// Plugin host (Luau)
// =================================================================================================
struct VmState
{
    size_t bytes = 0;
    size_t peak = 0;
    bool capHit = false;
    Clock::time_point deadline{};
};

void* capAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    auto* vm = static_cast<VmState*>(ud);
    if (nsize == 0)
    {
        vm->bytes -= osize;
        free(ptr);
        return nullptr;
    }
    if (nsize > osize && vm->bytes + (nsize - osize) > kVmMemoryCap)
    {
        vm->capHit = true;
        return nullptr; // Luau raises a memory error inside the script
    }
    void* block = realloc(ptr, nsize);
    if (block)
    {
        vm->bytes = vm->bytes - osize + nsize;
        vm->peak = std::max(vm->peak, vm->bytes);
    }
    return block;
}

int luaNow(lua_State* L)
{
    lua_pushnumber(L, qpcMicros() / 1e6);
    return 1;
}

struct Vm
{
    VmState state;
    lua_State* L = nullptr;
    lua_State* thread = nullptr;
    std::string loadError;
};

// Creates a sandboxed VM: base libs only, no loadstring, read-only globals, per-VM memory cap,
// interrupt that aborts any callback running longer than kCallbackBudget.
std::unique_ptr<Vm> createVm(const std::string& source)
{
    auto vm = std::make_unique<Vm>();
    vm->L = lua_newstate(capAlloc, &vm->state);
    luaL_openlibs(vm->L);
    lua_pushnil(vm->L);
    lua_setglobal(vm->L, "loadstring");
    lua_newtable(vm->L);
    lua_pushcfunction(vm->L, luaNow, "now");
    lua_setfield(vm->L, -2, "now");
    lua_setglobal(vm->L, "rmw");
    lua_callbacks(vm->L)->userdata = &vm->state;
    lua_callbacks(vm->L)->interrupt = [](lua_State* L, int gc) {
        if (gc >= 0)
        {
            return;
        }
        auto* s = static_cast<VmState*>(lua_callbacks(L)->userdata);
        if (Clock::now() > s->deadline)
        {
            luaL_error(L, "callback exceeded its time budget");
        }
    };
    luaL_sandbox(vm->L);
    vm->thread = lua_newthread(vm->L);
    luaL_sandboxthread(vm->thread);

    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
    const int loaded = luau_load(vm->thread, "=plugin", bytecode, bytecodeSize, 0);
    free(bytecode);
    vm->state.deadline = Clock::now() + kCallbackBudget;
    if (loaded != 0 || lua_pcall(vm->thread, 0, 0, 0) != 0)
    {
        vm->loadError = lua_tostring(vm->thread, -1) ? lua_tostring(vm->thread, -1) : "load error";
        lua_pop(vm->thread, 1);
    }
    return vm;
}

// Calls update() under the time budget; returns the string result or an error marker.
std::string callUpdate(Vm& vm)
{
    vm.state.deadline = Clock::now() + kCallbackBudget;
    lua_getglobal(vm.thread, "update");
    if (lua_pcall(vm.thread, 0, 1, 0) != 0)
    {
        std::string err = lua_tostring(vm.thread, -1) ? lua_tostring(vm.thread, -1) : "error";
        lua_pop(vm.thread, 1);
        return "!" + err;
    }
    std::string value = lua_tostring(vm.thread, -1) ? lua_tostring(vm.thread, -1) : "";
    lua_pop(vm.thread, 1);
    return value;
}

const char* kScripts[kWidgets] = {
    "local n = 0 function update() n = n + 1 return tostring(1 + n % 4) end",
    "local titles = {'Visual Studio Code', 'Firefox', 'Terminal'} local i = 0 function update() i = i % 3 + "
    "1 return titles[i] end",
    "function update() return 'lofi beats - track ' .. tostring(math.floor(rmw.now()) % 12) end",
    "function update() local t = math.floor(rmw.now()) return string.format('%02d:%02d:%02d', (t // 3600) % "
    "24, (t // 60) % 60, t % 60) end",
    "function update() return tostring(math.floor(20 + 15 * math.sin(rmw.now()))) end",
    "function update() return string.format('%.1fG', 6 + math.sin(rmw.now() / 7)) end",
    "function update() return string.format('%.0f KB/s', 300 + 200 * math.sin(rmw.now())) end",
    "local v = 40 function update() v = (v + 1) % 100 return tostring(v) .. '%' end",
    "function update() return '87%' end",
    "local c = 0 function update() c = (c + 1) % 9 return tostring(c) end",
};

int runHost(const std::wstring& pipeName)
{
    const double memStart = privateMb();
    std::vector<std::unique_ptr<Vm>> vms;
    for (const auto* script : kScripts)
    {
        vms.push_back(createVm(script));
    }
    const double memAfterVms = privateMb();

    // Hostile scripts: must be stopped without hurting the host.
    nlohmann::json hostile;
    {
        auto loop = createVm("function update() while true do end end");
        const auto start = Clock::now();
        const std::string r = callUpdate(*loop);
        hostile["infiniteLoop"] = {
            {"stopped", r.rfind('!', 0) == 0},
            {"afterMs", std::chrono::duration<double, std::milli>(Clock::now() - start).count()},
            {"error", r}};
    }
    {
        // 4 MB strings: reaches the 16 MB cap long before the 50 ms time budget.
        auto bomb = createVm(
            "function update() local t = {} for i = 1, 100 do t[i] = string.rep('x', 4194304) end end");
        const std::string r = callUpdate(*bomb);
        hostile["memoryBomb"] = {{"stopped", r.rfind('!', 0) == 0},
                                 {"capHit", bomb->state.capHit},
                                 {"peakMb", bomb->state.peak / 1048576.0},
                                 {"error", r}};
    }
    {
        auto escape = createVm("function update() return tostring(loadstring) .. ' ' .. tostring(io) .. ' ' "
                               ".. tostring(os and os.execute) end");
        hostile["escapeAttempts"] = callUpdate(*escape); // expect "nil nil nil"
    }
    {
        auto write = createVm("function update() string.upper = nil return 'modified' end");
        hostile["modifyBuiltins"] = callUpdate(*write); // expect a read-only error
    }

    wil::unique_hfile pipe(CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe)
    {
        return 1;
    }
    const auto send = [&](const std::string& line) {
        DWORD written = 0;
        return WriteFile(pipe.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) !=
               FALSE;
    };

    std::vector<double> updateMs;
    for (int round = 0;; ++round)
    {
        const auto start = Clock::now();
        nlohmann::json batch;
        std::vector<std::string> values;
        for (auto& vm : vms)
        {
            values.push_back(callUpdate(*vm));
        }
        updateMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        batch["v"] = values;
        batch["t"] = qpcMicros();
        if (!send(batch.dump() + "\n"))
        {
            break;
        }
        if (round == 8) // report early, the bar may terminate us at any time
        {
            size_t vmBytes = 0;
            for (auto& vm : vms)
            {
                vmBytes += vm->state.bytes;
            }
            nlohmann::json report{{"privateMbStart", memStart},
                                  {"privateMbAfter10Vms", memAfterVms},
                                  {"luauHeapKbPerVm", vmBytes / 1024.0 / vms.size()},
                                  {"updateAllMsP95", percentile(updateMs, 0.95)},
                                  {"loadErrors",
                                   [&] {
                                       std::vector<std::string> e;
                                       for (auto& vm : vms)
                                       {
                                           if (!vm->loadError.empty())
                                           {
                                               e.push_back(vm->loadError);
                                           }
                                       }
                                       return e;
                                   }()},
                                  {"hostile", hostile}};
            send(nlohmann::json{{"report", report}}.dump() + "\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring_view mode = argc > 1 ? argv[1] : L"bar";
    if (mode == L"host" && argc > 2)
    {
        return runHost(argv[2]);
    }
    return runBar(argc > 2 ? std::max(10, _wtoi(argv[2])) : 20);
}
