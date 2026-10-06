#include "SharedBlock.hpp"

#include <windows.h>

#include <sddl.h>

#include <array>
#include <cstddef>
#include <format>
#include <string>

namespace rmw::tilerapp
{
namespace
{

// An elevated process's objects are the administrators' alone by default. These are made the
// user's (and Windows'), at medium integrity: the person's own bar must open them.
class UserSecurity
{
public:
    UserSecurity()
    {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        {
            return;
        }
        std::array<std::byte, 256> buffer{};
        DWORD size = 0;
        const bool known = GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()),
                                               &size) != FALSE;
        CloseHandle(token);
        wchar_t* sid = nullptr;
        if (!known ||
            ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &sid) == FALSE)
        {
            return;
        }
        const std::wstring sddl = std::format(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{})S:(ML;;NW;;;ME)", sid);
        LocalFree(sid);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor,
                                                                 nullptr) != FALSE)
        {
            m_attributes.lpSecurityDescriptor = m_descriptor;
        }
    }
    ~UserSecurity() { LocalFree(m_descriptor); }
    UserSecurity(const UserSecurity&) = delete;
    UserSecurity& operator=(const UserSecurity&) = delete;
    UserSecurity(UserSecurity&&) = delete;
    UserSecurity& operator=(UserSecurity&&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept
    {
        return m_descriptor != nullptr ? &m_attributes : nullptr;
    }

private:
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES m_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};

} // namespace

SharedBlock::SharedBlock(bool elevated)
{
    UserSecurity security;
    SECURITY_ATTRIBUTES* const shared = elevated ? security.attributes() : nullptr;
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, shared, PAGE_READWRITE, 0, sizeof(tiling::SharedState),
                                   tiling::kStateMappingName);
    if (m_mapping != nullptr)
    {
        m_view = static_cast<tiling::SharedState*>(
            MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(tiling::SharedState)));
    }
    m_event = CreateEventW(shared, FALSE, FALSE, tiling::kStateEventName);
}

SharedBlock::~SharedBlock()
{
    if (m_view != nullptr)
    {
        begin();
        m_view->monitorCount = 0;
        end();
        UnmapViewOfFile(m_view);
    }
    if (m_mapping != nullptr)
    {
        CloseHandle(m_mapping);
    }
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
    }
}

void SharedBlock::begin() noexcept
{
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&m_view->sequence)); // odd: being written
}

void SharedBlock::end() noexcept
{
    m_view->version = tiling::kSharedVersion;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&m_view->sequence)); // even: done
    if (m_event != nullptr)
    {
        SetEvent(m_event);
    }
}

} // namespace rmw::tilerapp
