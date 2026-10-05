#include "rmw/platform/Process.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shlobj.h>
#include <wil/resource.h>

#include <sddl.h>

#include <algorithm>
#include <array>
#include <format>
#include <utility>
#include <vector>

namespace rmw::platform
{
namespace
{

Result<std::wstring> userSidString()
{
    wil::unique_handle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        return std::unexpected(lastError("OpenProcessToken"));
    }
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE)
    {
        return std::unexpected(lastError("GetTokenInformation"));
    }
    wil::unique_hlocal_string text;
    if (ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, text.put()) ==
        FALSE)
    {
        return std::unexpected(lastError("ConvertSidToStringSid"));
    }
    return std::wstring(text.get());
}

} // namespace

// ---- child process ----------------------------------------------------------------------------

Result<ChildProcess> ChildProcess::spawn(const std::filesystem::path& executable,
                                         const std::wstring& arguments, const ChildOptions& options)
{
    ChildProcess child;
    if (options.ownJob)
    {
        wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
        if (!job)
        {
            return std::unexpected(lastError("CreateJobObject"));
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
#if !defined(__SANITIZE_ADDRESS__)
        if (options.memoryLimitBytes > 0)
        {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = static_cast<SIZE_T>(options.memoryLimitBytes);
        }
#endif
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ==
            FALSE)
        {
            return std::unexpected(lastError("SetInformationJobObject"));
        }
        child.m_job = job.release();
    }

    // CreateProcess may write into the command line buffer.
    std::wstring commandLine = std::format(L"\"{}\" {}", executable.native(), arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (options.breakawayFromJob)
    {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                                  nullptr, nullptr, &startup, &info);
    if (created == FALSE && options.breakawayFromJob && GetLastError() == ERROR_ACCESS_DENIED)
    {
        // The parent's job forbids breakaway: start inside it rather than not at all.
        flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
        created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                                 nullptr, nullptr, &startup, &info);
    }
    if (created == FALSE)
    {
        return std::unexpected(lastError(std::format("start {}", executable.filename().string())));
    }
    const wil::unique_handle thread(info.hThread);
    child.m_process = info.hProcess;
    child.m_pid = info.dwProcessId;
    if (child.m_job != nullptr && AssignProcessToJobObject(child.m_job, info.hProcess) == FALSE)
    {
        Error error = lastError("AssignProcessToJobObject");
        child.terminate();
        return std::unexpected(std::move(error));
    }
    ResumeThread(thread.get());
    return child;
}

ChildProcess::~ChildProcess()
{
    if (m_job != nullptr)
    {
        CloseHandle(m_job); // KILL_ON_JOB_CLOSE ends the child
    }
    if (m_process != nullptr)
    {
        CloseHandle(m_process);
    }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : m_process(std::exchange(other.m_process, nullptr))
    , m_job(std::exchange(other.m_job, nullptr))
    , m_pid(std::exchange(other.m_pid, 0))
{
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
    if (this != &other)
    {
        const ChildProcess old(std::move(*this));
        m_process = std::exchange(other.m_process, nullptr);
        m_job = std::exchange(other.m_job, nullptr);
        m_pid = std::exchange(other.m_pid, 0);
    }
    return *this;
}

bool ChildProcess::running() const noexcept
{
    return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

std::optional<std::uint32_t> ChildProcess::wait(std::chrono::milliseconds timeout) const noexcept
{
    if (m_process == nullptr)
    {
        return std::nullopt;
    }
    const auto ms = static_cast<DWORD>(std::clamp<std::int64_t>(timeout.count(), 0, INFINITE - 1));
    if (WaitForSingleObject(m_process, ms) != WAIT_OBJECT_0)
    {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(m_process, &code);
    return code;
}

void ChildProcess::terminate() noexcept
{
    if (m_job != nullptr)
    {
        TerminateJobObject(m_job, 1);
    }
    else if (m_process != nullptr)
    {
        TerminateProcess(m_process, 1);
    }
}

void ChildProcess::detach() noexcept
{
    if (m_job != nullptr)
    {
        // Without KILL_ON_JOB_CLOSE the child survives our handle.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        CloseHandle(std::exchange(m_job, nullptr));
    }
    if (m_process != nullptr)
    {
        CloseHandle(std::exchange(m_process, nullptr));
    }
}

// ---- mitigations ------------------------------------------------------------------------------

Result<void> applyStrictMitigations() noexcept
{
#if defined(__SANITIZE_ADDRESS__)
    return {}; // ASan patches code and loads its runtime late
#else
    std::array<wchar_t, 8> flag{};
    if (GetEnvironmentVariableW(L"RMW_DISABLE_MITIGATIONS", flag.data(), static_cast<DWORD>(flag.size())) >
            0 &&
        flag[0] == L'1')
    {
        log::warn("process mitigations disabled by RMW_DISABLE_MITIGATIONS");
        return {};
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    dynamicCode.ProhibitDynamicCode = 1;
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};
    extensionPoints.DisableExtensionPoints = 1;
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    imageLoad.PreferSystem32Images = 1;
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    signature.MicrosoftSignedOnly = 1;

    struct Policy
    {
        PROCESS_MITIGATION_POLICY kind;
        const void* data;
        std::size_t size;
        const char* name;
    };
    const std::array<Policy, 4> policies = {{
        {ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode), "dynamic code"},
        {ProcessExtensionPointDisablePolicy, &extensionPoints, sizeof(extensionPoints), "extension points"},
        {ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad), "image load"},
        {ProcessSignaturePolicy, &signature, sizeof(signature), "signature"},
    }};
    for (const auto& policy : policies)
    {
        if (SetProcessMitigationPolicy(policy.kind, const_cast<void*>(policy.data), policy.size) == FALSE)
        {
            return std::unexpected(lastError(std::format("mitigation policy '{}'", policy.name)));
        }
    }
    return {};
#endif
}

// ---- single instance and paths ---------------------------------------------------------------------

Result<SingleInstance> SingleInstance::acquire(std::wstring_view purpose)
{
    auto sid = userSidString();
    if (!sid)
    {
        return std::unexpected(std::move(sid.error()));
    }
    // "Local\" already scopes the name to this session.
    const std::wstring name = std::format(L"Local\\RicingManagerWindows-{}-{}", purpose, *sid);
    SingleInstance instance;
    instance.m_mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (instance.m_mutex == nullptr)
    {
        return std::unexpected(lastError("CreateMutex"));
    }
    instance.m_owned = GetLastError() != ERROR_ALREADY_EXISTS;
    return instance;
}

SingleInstance::~SingleInstance()
{
    if (m_mutex != nullptr)
    {
        if (m_owned)
        {
            ReleaseMutex(m_mutex);
        }
        CloseHandle(m_mutex);
    }
}

SingleInstance::SingleInstance(SingleInstance&& other) noexcept
    : m_mutex(std::exchange(other.m_mutex, nullptr))
    , m_owned(std::exchange(other.m_owned, false))
{
}

Result<std::filesystem::path> defaultDataDirectory()
{
    wil::unique_cotaskmem_string localAppData;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, localAppData.put())))
    {
        return fail(ErrorCode::System, "cannot find %LOCALAPPDATA%");
    }
    return std::filesystem::path(localAppData.get()) / L"RicingManagerWindows";
}

} // namespace rmw::platform
