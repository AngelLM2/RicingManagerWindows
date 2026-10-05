#pragma once

#include "rmw/engine/Module.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Process.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

// `rmw-core --apply-helper` (docs/03 §2.3, §5): calls that can hang with Explorer run in a child
// process inside a job. The core is the pipe server and RPC client; a call that does not answer
// within the timeout ends the child, and the next call starts a fresh one.
namespace rmw::core
{

inline constexpr std::chrono::milliseconds kHelperTimeout = std::chrono::seconds(15);
inline constexpr std::uint64_t kHelperMemoryLimit = 1ull << 30;

class HelperModuleRunner final : public engine::IModuleRunner
{
public:
    explicit HelperModuleRunner(std::filesystem::path coreExecutable,
                                std::chrono::milliseconds timeout = kHelperTimeout);

    [[nodiscard]] Result<engine::CaptureResult> capture(std::string_view moduleId,
                                                        std::span<const std::string> fields) override;
    [[nodiscard]] Result<void> apply(std::string_view moduleId, const engine::Json& desired) override;

    // Ends the helper now (idle core, shutdown).
    void stop() noexcept;

private:
    [[nodiscard]] Result<engine::Json> call(std::string_view method, const engine::Json& params);
    [[nodiscard]] Result<void> start();

    std::filesystem::path m_executable;
    std::chrono::milliseconds m_timeout;
    std::unique_ptr<platform::ChildProcess> m_child;
    std::unique_ptr<platform::PipeConnection> m_connection;
    std::int64_t m_nextId = 1;
};

// The helper's main loop. Returns the process exit code.
[[nodiscard]] int runApplyHelper(const std::wstring& pipeName);

// CaptureResult <-> JSON for the wire.
[[nodiscard]] engine::Json captureToJson(const engine::CaptureResult& capture);
[[nodiscard]] Result<engine::CaptureResult> captureFromJson(const engine::Json& json);

} // namespace rmw::core
