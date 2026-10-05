#include "rmw/platform/ProcessHardening.hpp"

#include "rmw/platform/WinError.hpp"

#include <windows.h>

namespace rmw::platform
{

Result<void> hardenProcess() noexcept
{
    try
    {
        if (SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) == FALSE)
        {
            return std::unexpected(lastError("SetDefaultDllDirectories"));
        }

        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strictHandles{};
        strictHandles.RaiseExceptionOnInvalidHandleReference = 1;
        strictHandles.HandleExceptionsPermanentlyEnabled = 1;
        if (SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &strictHandles,
                                       sizeof(strictHandles)) == FALSE)
        {
            return std::unexpected(lastError("SetProcessMitigationPolicy(StrictHandleCheck)"));
        }
        return {};
    }
    catch (...)
    {
        return fail(ErrorCode::Unknown, "hardenProcess: unexpected exception");
    }
}

} // namespace rmw::platform
