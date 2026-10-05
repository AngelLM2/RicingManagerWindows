#pragma once

#include "ApplyHelper.hpp"
#include "Composition.hpp"

#include "rmw/engine/RiceEngine.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/Pipe.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rmw::core
{

struct CoreOptions
{
    std::filesystem::path dataDirectory;
    // Exit after this long without clients (zero-resident mode, docs/03 §2.2).
    std::chrono::seconds idleExit{30};
    // Run the module system halves in this process instead of the apply helper (debugging).
    bool inProcess = false;

    // System-test hooks (set only by --test-* flags): hold every operation for a while, or
    // pause right after one module is applied (to kill the core mid-operation).
    std::chrono::milliseconds testOperationDelay{0};
    std::string testPauseAfterModule;
    std::chrono::milliseconds testPause{0};
};

// The single writer (docs/03 §2.1): owns state.db, the store and the engine; clients talk to it
// over the core pipe. One operation at a time — a second one gets `busy`.
class CoreServer
{
public:
    explicit CoreServer(CoreOptions options);
    ~CoreServer();
    CoreServer(const CoreServer&) = delete;
    CoreServer& operator=(const CoreServer&) = delete;
    CoreServer(CoreServer&&) = delete;
    CoreServer& operator=(CoreServer&&) = delete;

    // Opens the state, builds the modules and undoes an interrupted operation.
    [[nodiscard]] Result<void> initialize();
    // Serves until idle or asked to shut down. Returns the process exit code.
    [[nodiscard]] int serve(platform::PipeServer& server);

    // Startup mode: only recover, no serving (docs/03 §2.2 `--startup`).
    [[nodiscard]] Result<bool> recoverOnly();

private:
    void handleClient(platform::PipeConnection connection);
    [[nodiscard]] Result<ipc::Json> dispatch(const ipc::Request& request);
    [[nodiscard]] Result<ipc::Json> exclusive(const std::function<Result<ipc::Json>()>& operation);

    [[nodiscard]] Result<ipc::Json> status();
    [[nodiscard]] Result<ipc::Json> validateRice(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> applyRice(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> restore(const ipc::Json& params, bool original);
    [[nodiscard]] Result<ipc::Json> monitors();
    [[nodiscard]] Result<ipc::Json> runAction(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> inspect(const ipc::Json& params);
    [[nodiscard]] engine::IModuleRunner& runner() noexcept;

    CoreOptions m_options;
    std::unique_ptr<ModuleLogic> m_logic;
    std::unique_ptr<ModuleSystems> m_systems; // --in-process only
    std::unique_ptr<HelperModuleRunner> m_helper;
    std::unique_ptr<engine::StateDb> m_state;
    std::unique_ptr<engine::ContentStore> m_store;
    std::unique_ptr<engine::RiceEngine> m_engine;

    std::mutex m_operation; // one engine operation at a time
    std::atomic<int> m_clients{0};
    std::atomic<bool> m_stop{false};
    std::atomic<std::chrono::steady_clock::rep> m_lastActivity{0};
    struct Worker
    {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::vector<Worker> m_workers;
};

} // namespace rmw::core
