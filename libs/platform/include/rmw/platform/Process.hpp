#pragma once

#include "rmw/base/Error.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rmw::platform
{

struct ChildOptions
{
    // Hard ceiling on committed memory (0 = none). Disabled under ASan, which reserves terabytes.
    std::uint64_t memoryLimitBytes = 0;
    // Start outside any job the parent belongs to (the core must outlive the terminal that
    // started it, docs/03 §2.1).
    bool breakawayFromJob = false;
    // Put the child in a job of its own (workers)...
    bool ownJob = true;
    // ...that kills it when our handle closes. Off for a worker that must clean up after the core
    // is gone (the bar shows Windows' taskbar again): it watches the core and exits by itself.
    bool killOnClose = true;
    // The job reports JOB_OBJECT_MSG_NOTIFICATION_LIMIT once committed memory passes this (0 =
    // none): the hard ceiling only makes allocations fail, it never ends the process (docs/03 §2.4).
    std::uint64_t memoryNotifyBytes = 0;
    // Job messages (memory, exit) go to this I/O completion port with this key (ownJob only).
    void* completionPort = nullptr;
    std::uintptr_t completionKey = 0;
    // The only handles the child inherits (each must be inheritable). Empty = none.
    std::vector<void*> inheritHandles;
    // ownJob only: the child cannot start processes of its own...
    bool noChildProcesses = false;
    // ...or touch the clipboard, global atoms, system settings, the display settings, other
    // desktops, or log off (JOB_OBJECT_UILIMIT_*; it can still read other windows' titles).
    bool restrictUi = false;
};

// A child process (RAII). With ownJob, destroying the object ends the child: workers never
// outlive the core (docs/03 §2.3).
class ChildProcess
{
public:
    [[nodiscard]] static Result<ChildProcess> spawn(const std::filesystem::path& executable,
                                                    const std::wstring& arguments,
                                                    const ChildOptions& options);

    ChildProcess() noexcept = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    [[nodiscard]] std::uint32_t pid() const noexcept { return m_pid; }
    [[nodiscard]] bool running() const noexcept;
    // Waits for exit; nullopt on timeout.
    [[nodiscard]] std::optional<std::uint32_t> wait(std::chrono::milliseconds timeout) const noexcept;
    // Ends the child (and its job) immediately.
    void terminate() noexcept;
    // Lets the child live on its own (closes our handles without killing it).
    void detach() noexcept;

private:
    void* m_process = nullptr; // HANDLE
    void* m_job = nullptr;     // HANDLE
    std::uint32_t m_pid = 0;
};

// The strict policies of ADR-0002 for the core and its apply helper: no dynamic code (ACG), no
// extension points, no remote/low-label images and only Microsoft-signed DLLs from now on. Set
// RMW_DISABLE_MITIGATIONS=1 to skip them while diagnosing a machine; never applied under ASan.
[[nodiscard]] Result<void> applyStrictMitigations() noexcept;

// Named mutex "Local\RicingManagerWindows-<SID>": true when this process now owns it.
class SingleInstance
{
public:
    [[nodiscard]] static Result<SingleInstance> acquire(std::wstring_view purpose);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&& other) noexcept;
    SingleInstance& operator=(SingleInstance&&) = delete;

    [[nodiscard]] bool owned() const noexcept { return m_owned; }

private:
    SingleInstance() noexcept = default;
    void* m_mutex = nullptr;
    bool m_owned = false;
};

// %LOCALAPPDATA%\RicingManagerWindows
[[nodiscard]] Result<std::filesystem::path> defaultDataDirectory();

} // namespace rmw::platform
