#include "VirtualDesktops.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <shobjidl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

namespace rmw::tilerapp
{
namespace
{

constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr const wchar_t* kParentKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
constexpr std::size_t kMaxDesktops = 64;

IVirtualDesktopManager* manager(void* raw) noexcept
{
    return static_cast<IVirtualDesktopManager*>(raw);
}

std::vector<std::uint8_t> binaryValue(HKEY key, const wchar_t* subKey, const wchar_t* name)
{
    DWORD size = 0;
    if (RegGetValueW(key, subKey, name, RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size > kMaxDesktops * 16)
    {
        return {};
    }
    std::vector<std::uint8_t> bytes(size);
    if (RegGetValueW(key, subKey, name, RRF_RT_REG_BINARY, nullptr, bytes.data(), &size) != ERROR_SUCCESS)
    {
        return {};
    }
    bytes.resize(size);
    return bytes;
}

VirtualDesktops::Id idOf(const GUID& guid) noexcept
{
    VirtualDesktops::Id id{};
    std::memcpy(id.data(), &guid, id.size());
    return id;
}

void press(std::vector<INPUT>& inputs, WORD vk, bool up)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0U) |
                       ((vk == VK_LEFT || vk == VK_RIGHT || vk == VK_LWIN) ? KEYEVENTF_EXTENDEDKEY : 0U);
    input.ki.dwExtraInfo = kOurKeys;
    inputs.push_back(input);
}

} // namespace

std::uint32_t VirtualDesktops::slide(bool next, int times) noexcept
{
    if (times <= 0)
    {
        return 0;
    }
    try
    {
        const bool ctrlHeld = (static_cast<unsigned short>(GetAsyncKeyState(VK_CONTROL)) & 0x8000U) != 0;
        const bool winHeld = (static_cast<unsigned short>(GetAsyncKeyState(VK_LWIN)) & 0x8000U) != 0 ||
                             (static_cast<unsigned short>(GetAsyncKeyState(VK_RWIN)) & 0x8000U) != 0;
        std::vector<INPUT> inputs;
        if (!ctrlHeld)
        {
            press(inputs, VK_LCONTROL, false);
        }
        if (!winHeld)
        {
            press(inputs, VK_LWIN, false);
        }
        for (int i = 0; i < times; ++i)
        {
            press(inputs, next ? VK_RIGHT : VK_LEFT, false);
            press(inputs, next ? VK_RIGHT : VK_LEFT, true);
        }
        if (!winHeld)
        {
            // An unassigned key before Win goes up: Windows sees a Win shortcut, not a lone Win.
            press(inputs, 0xE8, false);
            press(inputs, 0xE8, true);
            press(inputs, VK_LWIN, true);
        }
        if (!ctrlHeld)
        {
            press(inputs, VK_LCONTROL, true);
        }
        if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size())
        {
            return inputs.back().ki.wVk;
        }
    }
    catch (...)
    {
        log::warn("tiler: could not switch the virtual desktop");
    }
    return 0;
}

std::unique_ptr<VirtualDesktops> VirtualDesktops::create()
{
    IVirtualDesktopManager* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&raw))))
    {
        log::warn("tiler: Windows' virtual desktops are not available here");
        return nullptr;
    }
    std::unique_ptr<VirtualDesktops> desktops(new VirtualDesktops());
    desktops->m_manager = raw;
    desktops->m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    desktops->rearm();
    return desktops;
}

VirtualDesktops::~VirtualDesktops()
{
    if (m_manager != nullptr)
    {
        manager(m_manager)->Release();
    }
    if (m_key != nullptr)
    {
        RegCloseKey(static_cast<HKEY>(m_key));
    }
    if (m_event != nullptr)
    {
        CloseHandle(m_event);
    }
}

void VirtualDesktops::rearm() noexcept
{
    if (m_event == nullptr)
    {
        return;
    }
    // Explorer makes its key the first time someone has two desktops: until then, its parent is
    // watched (only read, never written).
    if (!m_ownKey)
    {
        HKEY key = nullptr;
        const bool own = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ | KEY_NOTIFY, &key) == ERROR_SUCCESS;
        if (!own &&
            RegOpenKeyExW(HKEY_CURRENT_USER, kParentKey, 0, KEY_READ | KEY_NOTIFY, &key) != ERROR_SUCCESS)
        {
            return;
        }
        if (m_key != nullptr)
        {
            RegCloseKey(static_cast<HKEY>(m_key));
        }
        m_key = key;
        m_ownKey = own;
    }
    RegNotifyChangeKeyValue(static_cast<HKEY>(m_key), TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                            m_event, TRUE);
}

VirtualDesktops::State VirtualDesktops::read() const
{
    State state;
    const auto ids = binaryValue(HKEY_CURRENT_USER, kKey, L"VirtualDesktopIDs");
    for (std::size_t at = 0; at + 16 <= ids.size(); at += 16)
    {
        Id id{};
        std::copy_n(ids.begin() + static_cast<std::ptrdiff_t>(at), id.size(), id.begin());
        state.desktops.push_back(id);
    }
    if (state.desktops.empty())
    {
        state.desktops.push_back(Id{}); // never had two: the one there is
        return state;
    }
    auto current = binaryValue(HKEY_CURRENT_USER, kKey, L"CurrentVirtualDesktop");
    if (current.size() != 16)
    {
        // Windows 10 keeps it per session.
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        const std::wstring perSession = std::format(
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\{}\\VirtualDesktops", session);
        current = binaryValue(HKEY_CURRENT_USER, perSession.c_str(), L"CurrentVirtualDesktop");
    }
    if (current.size() == 16)
    {
        Id id{};
        std::ranges::copy(current, id.begin());
        const auto found = std::ranges::find(state.desktops, id);
        state.current = found == state.desktops.end() ? 0 : static_cast<int>(found - state.desktops.begin());
    }
    return state;
}

std::optional<int> VirtualDesktops::desktopOf(Window window, const State& state) const
{
    GUID guid{};
    if (FAILED(manager(m_manager)->GetWindowDesktopId(static_cast<HWND>(window), &guid)))
    {
        return std::nullopt;
    }
    const Id id = idOf(guid);
    if (state.desktops.size() == 1 && state.desktops.front() == Id{})
    {
        return 0;
    }
    const auto found = std::ranges::find(state.desktops, id);
    if (found == state.desktops.end())
    {
        return std::nullopt; // on every desktop, or just made
    }
    return static_cast<int>(found - state.desktops.begin());
}

int VirtualDesktops::slidesTo(int index, const State& state) noexcept
{
    const int last = static_cast<int>(state.desktops.size()) - 1;
    return std::clamp(index, 0, std::max(last, 0)) - state.current;
}

void VirtualDesktops::goTo(int index, const State& state) noexcept
{
    const int slides = slidesTo(index, state);
    slide(slides > 0, std::abs(slides));
}

} // namespace rmw::tilerapp
