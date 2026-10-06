#include "WinKeyHook.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <array>

namespace rmw::barapp
{
namespace
{

// An unassigned virtual key: pressing it changes nothing anywhere, but Windows sees that Win was
// not alone.
constexpr WORD kMaskKey = 0xE8;
// What marks the keys we send ourselves (any other program's, a test's, an AutoHotkey script's,
// count like the user's own).
constexpr ULONG_PTR kOurs = 0x524D5753; // "RMWS"
constexpr UINT kSendMask = WM_APP + 1;  // to the hook's thread: wParam = the Win key

WinKeyHook* g_hook = nullptr; // the one hook of this process

bool isWin(std::uint32_t virtualKey) noexcept
{
    return virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

} // namespace

WinKeyTracker::Verdict WinKeyTracker::key(std::uint32_t virtualKey, bool down) noexcept
{
    if (isWin(virtualKey))
    {
        if (down)
        {
            if (!m_winDown)
            {
                m_winDown = true;
                m_combined = false;
            }
            return Verdict::Pass; // auto-repeat, or the first press
        }
        const bool alone = m_winDown && !m_combined;
        m_winDown = false;
        return alone ? Verdict::OpenOurs : Verdict::Pass;
    }
    if (m_winDown && down)
    {
        m_combined = true;
    }
    return Verdict::Pass;
}

struct WinKeyHook::Access
{
    static LRESULT CALLBACK proc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && g_hook != nullptr)
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the hook's contract
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if (event->dwExtraInfo != kOurs)
            {
                const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
                if (g_hook->m_tracker.key(event->vkCode, down) == WinKeyTracker::Verdict::OpenOurs)
                {
                    // The release waits: the mask goes first, then it (from the loop, just after).
                    PostThreadMessageW(GetCurrentThreadId(), kSendMask, event->vkCode, 0);
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
};

WinKeyHook::WinKeyHook(std::function<void()> open)
    : m_open(std::move(open))
{
    if (g_hook != nullptr)
    {
        log::warn("bar: the Win key hook is already there");
        return;
    }
    g_hook = this;
    m_thread = std::jthread([this] { run(); });
}

WinKeyHook::~WinKeyHook()
{
    if (const auto thread = m_threadId.load(); thread != 0)
    {
        PostThreadMessageW(thread, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    if (g_hook == this)
    {
        g_hook = nullptr;
    }
}

void WinKeyHook::run()
{
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's queue, first
    m_threadId = GetCurrentThreadId();
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &Access::proc, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr)
    {
        log::warn("bar: cannot watch the Win key ({}); it stays Windows'", GetLastError());
        return;
    }
    m_active = true;
    log::info("bar: the Win key opens our Start");
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message == kSendMask)
        {
            // Mask, then the Win release itself: Windows sees a Win "shortcut" and keeps its Start
            // closed; and having just sent input, this process may bring its menu to the front.
            std::array<INPUT, 3> inputs{};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wVk = kMaskKey;
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki.wVk = kMaskKey;
            inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            inputs[2].type = INPUT_KEYBOARD;
            inputs[2].ki.wVk = static_cast<WORD>(message.wParam);
            inputs[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
            for (auto& input : inputs)
            {
                input.ki.dwExtraInfo = kOurs;
            }
            SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
            if (m_open)
            {
                m_open();
            }
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook);
    m_active = false;
}

} // namespace rmw::barapp
