#include "rmw/base/FailFast.hpp"

#include "rmw/base/Log.hpp"

#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace rmw
{

void failFast(std::string_view reason, std::source_location where) noexcept
{
    log::instance().write(log::Level::Critical, reason, where);
    log::instance().flush();

#if defined(_MSC_VER)
    // 7 == FAST_FAIL_FATAL_APP_EXIT (winnt.h); kept literal so base stays free of Windows headers.
    __fastfail(7);
#else
    std::abort();
#endif
}

} // namespace rmw
