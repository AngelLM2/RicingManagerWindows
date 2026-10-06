#include "KeyHook.hpp"

#include "WindowOps.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/tiling/Plan.hpp"

#include <windows.h>

#include <array>

namespace rmw::tilerapp
{
namespace
{

constexpr UINT kSendMask = WM_APP + 1; // to the hook's thread

KeyHook* g_hook = nullptr; // the one hook of this process

bool isModifier(DWORD vk) noexcept
{
    switch (vk)
    {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool down(int vk) noexcept
{
    return (static_cast<unsigned short>(GetAsyncKeyState(vk)) & 0x8000U) != 0;
}

ModifierKeys heldNow() noexcept
{
    return ModifierKeys{down(VK_LCONTROL),
                        down(VK_RCONTROL),
                        down(VK_LMENU),
                        down(VK_RMENU),
                        down(VK_LSHIFT) || down(VK_RSHIFT),
                        down(VK_LWIN) || down(VK_RWIN)};
}

} // namespace

std::uint32_t ModifierKeys::modifiers() const noexcept
{
    std::uint32_t out = 0;
    const bool altGr = rightAlt && leftCtrl && !leftAlt && !rightCtrl;
    if (!altGr)
    {
        if (leftCtrl || rightCtrl)
        {
            out |= MOD_CONTROL;
        }
        if (leftAlt || rightAlt)
        {
            out |= MOD_ALT;
        }
    }
    if (shift)
    {
        out |= MOD_SHIFT;
    }
    if (win)
    {
        out |= MOD_WIN;
    }
    return out;
}

struct KeyHook::Access
{
    static LRESULT CALLBACK proc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && g_hook != nullptr)
        {
            // NOLINTNEXTLINE(performance-no-int-to-ptr): the hook's contract
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if (event->dwExtraInfo != kOurKeys && !isModifier(event->vkCode) && event->vkCode < 256)
            {
                const bool pressed = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
                if (!pressed && g_hook->m_swallowed[event->vkCode])
                {
                    g_hook->m_swallowed[event->vkCode] = false;
                    return 1;
                }
                if (pressed)
                {
                    const std::uint32_t modifiers = heldNow().modifiers();
                    const auto keys = g_hook->m_keys.load();
                    const auto match = modifiers != 0 && keys
                                           ? tiling::matchKey(*keys, modifiers, event->vkCode)
                                           : std::nullopt;
                    if (match)
                    {
                        g_hook->m_swallowed[event->vkCode] = true;
                        PostMessageW(static_cast<HWND>(g_hook->m_target), kCommandMessage,
                                     static_cast<WPARAM>(match->command), match->number);
                        if ((modifiers & MOD_WIN) != 0)
                        {
                            PostThreadMessageW(GetCurrentThreadId(), kSendMask, 0, 0);
                        }
                        return 1;
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
};

KeyHook::KeyHook(void* target)
    : m_target(target)
{
    if (g_hook != nullptr)
    {
        log::warn("tiler: the shortcut hook is already there");
        return;
    }
    g_hook = this;
    m_thread = std::jthread([this] { run(); });
}

KeyHook::~KeyHook()
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

void KeyHook::setKeys(Keys keys)
{
    m_keys.store(std::make_shared<const Keys>(std::move(keys)));
}

void KeyHook::run()
{
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's queue, first
    m_threadId = GetCurrentThreadId();
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &Access::proc, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr)
    {
        log::warn("tiler: cannot watch the keyboard ({}); the shortcuts do nothing", GetLastError());
        return;
    }
    m_active = true;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message == kSendMask)
        {
            // From here, never from inside the hook: SendInput there runs the hooks again.
            std::array<INPUT, 2> inputs{};
            for (auto& input : inputs)
            {
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = 0xE8;
                input.ki.dwExtraInfo = kOurKeys;
            }
            inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook);
    m_active = false;
}

} // namespace rmw::tilerapp
