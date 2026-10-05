#pragma once

#include "ApplyHelper.hpp"
#include "Composition.hpp"

#include "rmw/domain/Settings.hpp"
#include "rmw/engine/RiceEngine.hpp"
#include "rmw/ipc/JsonRpc.hpp"
#include "rmw/platform/MessageWindow.hpp"
#include "rmw/platform/Pipe.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rmw::core
{

class Resident;

struct CoreOptions
{
    std::filesystem::path dataDirectory;
    // Exit after this long without clients when nothing needs the core (docs/03 §2.2).
    std::chrono::seconds idleExit{30};
    // Run the module system halves in this process instead of the apply helper (debugging).
    bool inProcess = false;
    // Started at logon (`--startup`): stays only if something needs it.
    bool startup = false;
    // `--safe-mode`, or Shift held at logon (docs/06 §6).
    bool safeModeRequested = false;

    // System-test hooks (set only by --test-* flags): hold every operation for a while, or
    // pause right after one module is applied (to kill the core mid-operation).
    std::chrono::milliseconds testOperationDelay{0};
    std::string testPauseAfterModule;
    std::chrono::milliseconds testPause{0};
};

// Actions are what hotkeys, rules, the tray, the CLI and (later) plugins call (docs/12).
inline constexpr std::array<std::string_view, 5> kActions = {
    "rice.apply", "restore.original", "appearance.toggle", "appearance.dark", "appearance.light",
};

// Who asked: hotkeys, the tray and clients are the user (a history entry the user can go back
// to); rules and the schedule are automatic (no history, nothing done if already so).
enum class Origin
{
    User,
    Automatic,
};

enum class SafeModeReason
{
    None,
    Requested,
    Crashes, // the last starts died before a minute of health (docs/06 §6)
};

// The single writer (docs/03 §2.1): owns state.db, the store and the engine; clients talk to it
// over the core pipe. One operation at a time — a client gets `busy`, automatic work waits.
//
// Threads (docs/03 §9): the main thread runs the hidden window (system events, hotkeys, tray,
// timers); one thread accepts pipe clients, one per connected client; one work queue runs the
// resident's automatic operations so the window never blocks.
class CoreServer
{
public:
    static constexpr int kSafeModeAfterCrashes = 3;

    explicit CoreServer(CoreOptions options);
    ~CoreServer();
    CoreServer(const CoreServer&) = delete;
    CoreServer& operator=(const CoreServer&) = delete;
    CoreServer(CoreServer&&) = delete;
    CoreServer& operator=(CoreServer&&) = delete;

    // Counts the start (safe mode after repeated crashes), opens the state, builds the modules
    // and undoes an interrupted operation (not in safe mode).
    [[nodiscard]] Result<void> initialize();
    // After `--startup`: whether anything needs the core running (settings, schedule, safe mode).
    [[nodiscard]] bool wantsResidency() const;
    // Runs the window and serves the pipe until idle or shut down. Returns the exit code.
    [[nodiscard]] int run(platform::PipeServer& server);
    // This start went fine: the next one is not counted as a crash.
    void markHealthy() const noexcept;

    // ---- for the resident part, on the work queue: each waits for the running operation ----
    [[nodiscard]] Result<ipc::Json> perform(const domain::ActionCall& action, Origin origin);
    // Fields of `module` changed outside since we set them: no longer managed. Returns them.
    [[nodiscard]] Result<std::vector<std::string>> detachDrifted(const std::string& module);
    [[nodiscard]] Result<void> reapply(const std::vector<std::string>& modules);
    // Light or dark as the schedule says now. True when it changed something.
    [[nodiscard]] Result<bool> syncSchedule();
    // The next page of the slideshow the core turns on one monitor ("monitor:<key>").
    [[nodiscard]] Result<void> turnPage(const std::string& field);
    [[nodiscard]] Result<std::vector<std::string>> monitorKeys();

    // ---- any thread ----
    [[nodiscard]] std::optional<domain::AppearanceSchedule> schedule() const;
    // Slideshows the core turns: {"monitor:<key>": {"items", "intervalMs", "shuffle"}}.
    [[nodiscard]] ipc::Json rotation() const;
    [[nodiscard]] SafeModeReason safeMode() const noexcept { return m_safeMode; }
    // An operation is running or ended moments ago: change events are probably our own.
    [[nodiscard]] bool ownChangesSettling() const noexcept;
    [[nodiscard]] bool idleExpired() noexcept;
    [[nodiscard]] const std::filesystem::path& dataDirectory() const noexcept
    {
        return m_options.dataDirectory;
    }

private:
    struct OperationMark;

    void acceptLoop(platform::PipeServer& server, const std::stop_token& stop);
    void handleClient(platform::PipeConnection connection);
    [[nodiscard]] Result<ipc::Json> dispatch(const ipc::Request& request);
    // `changesWindows`: false for reads (inspect, export), which must not hide change events.
    [[nodiscard]] Result<ipc::Json> exclusive(const std::function<Result<ipc::Json>()>& operation,
                                              bool changesWindows = true);
    [[nodiscard]] Result<ipc::Json> onWindow(const std::function<ipc::Json()>& task);
    void notifyResident();

    [[nodiscard]] Result<ipc::Json> status();
    [[nodiscard]] Result<ipc::Json> validateRice(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> runAction(std::string_view name, const ipc::Json& params, Origin origin);
    [[nodiscard]] Result<ipc::Json> applyRice(const ipc::Json& params, Origin origin);
    [[nodiscard]] Result<ipc::Json> setAppearance(std::string_view action, Origin origin);
    [[nodiscard]] Result<ipc::Json> restore(const ipc::Json& params, bool original);
    [[nodiscard]] Result<ipc::Json> monitors();
    [[nodiscard]] Result<ipc::Json> inspect(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> exportRice(const ipc::Json& params);
    [[nodiscard]] Result<ipc::Json> autostart(const ipc::Json& params, bool change);
    [[nodiscard]] engine::IModuleRunner& runner() noexcept;
    void setSchedule(std::optional<domain::AppearanceSchedule> schedule);
    void setRotation(const ipc::Json& plan);

    CoreOptions m_options;
    SafeModeReason m_safeMode = SafeModeReason::None;
    std::unique_ptr<ModuleLogic> m_logic;
    std::unique_ptr<ModuleSystems> m_systems; // --in-process only
    std::unique_ptr<HelperModuleRunner> m_helper;
    std::unique_ptr<engine::StateDb> m_state;
    std::unique_ptr<engine::ContentStore> m_store;
    std::unique_ptr<engine::RiceEngine> m_engine;

    std::mutex m_operation; // one engine operation at a time
    std::atomic<bool> m_operationRunning{false};
    std::atomic<std::chrono::steady_clock::rep> m_lastOperationEnd{0};

    mutable std::mutex m_scheduleMutex;
    std::optional<domain::AppearanceSchedule> m_schedule; // mirrors meta "appearance_schedule"
    ipc::Json m_rotation = ipc::Json::object();           // mirrors meta "wallpaper_rotation"
    std::mt19937 m_random{std::random_device{}()};        // shuffled slideshows (operation lock)

    // Set before the accept thread starts and cleared after every client thread is joined.
    platform::MessageWindow* m_window = nullptr;
    Resident* m_resident = nullptr;

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
