#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/ipc/JsonRpc.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace rmw::platform
{
class PipeConnection;
}

// A client of rmw-core over its pipe (docs/03 §8): the CLI and the settings app. Starts the core
// when none runs, replaces one of another build, checks that it serves the same data folder.
namespace rmw::client
{

class CoreClient
{
public:
    // `dataDirectory` empty: the core's default folder.
    explicit CoreClient(std::filesystem::path dataDirectory = {});
    ~CoreClient();
    CoreClient(const CoreClient&) = delete;
    CoreClient& operator=(const CoreClient&) = delete;
    CoreClient(CoreClient&&) noexcept;
    CoreClient& operator=(CoreClient&&) noexcept;

    // `startIfNeeded` false: only a core that runs already (NotFound otherwise).
    [[nodiscard]] Result<void> connect(bool startIfNeeded = true);
    [[nodiscard]] bool connected() const noexcept { return m_connection != nullptr; }
    void disconnect() noexcept;

    // One request, one reply (calls are not concurrent: one client per thread).
    [[nodiscard]] Result<ipc::Json> call(std::string_view method, const ipc::Json& params,
                                         std::chrono::milliseconds timeout);

    // What the core said in its hello: its data folder, version.
    [[nodiscard]] const ipc::Json& hello() const noexcept { return m_hello; }

private:
    [[nodiscard]] Result<void> checkDataDirectory(const ipc::Json& hello) const;
    [[nodiscard]] Result<void> connectOrStart();

    std::filesystem::path m_dataDirectory;
    std::filesystem::path m_coreExe;
    std::wstring m_pipe;
    bool m_startIfNeeded = true;
    std::unique_ptr<platform::PipeConnection> m_connection;
    ipc::Json m_hello;
    std::int64_t m_nextId = 1;
};

} // namespace rmw::client
