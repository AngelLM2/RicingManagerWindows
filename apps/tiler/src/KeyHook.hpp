#pragma once

#include "rmw/domain/Settings.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace rmw::tilerapp
{

// The modifiers held now (MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8), from the keys that are
// down. AltGr (Right Alt plus the left Ctrl Windows makes up for it) is neither Ctrl nor Alt: it
// types ł, µ and ² on many keyboards, ABNT2's included.
struct ModifierKeys
{
    bool leftCtrl = false;
    bool rightCtrl = false;
    bool leftAlt = false;
    bool rightAlt = false;
    bool shift = false;
    bool win = false;

    [[nodiscard]] std::uint32_t modifiers() const noexcept;
};

// The tiler's shortcuts (M13): a low-level keyboard hook on a thread of its own (like the bar's Win
// key), so a busy tiler never slows typing nor gets the hook dropped. A shortcut's key is swallowed
// (with its release) and posted to `target` as kCommandMessage; nothing else is touched. After a
// shortcut with Win, an unassigned key goes out, so Windows (and our Start's Win key) see a Win
// shortcut and keep the Start menu closed.
//
// Until the target answers a command, the modifiers the person lets go wait (and go out after what the
// answer sends): a quick tap of Ctrl+Win+1 is over before the command arrives, and Ctrl+Win+arrow sent
// then would reach Windows as a bare arrow, the unassigned key after a lone Win (Start open).
class KeyHook
{
public:
    using Keys = std::vector<domain::TilingKey>;
    // To the target window: wParam = the domain::TilingCommand, lParam = its number (1..9) or 0.
    // The target answers each one with answer().
    static constexpr std::uint32_t kCommandMessage = 0x8000 + 0x52; // WM_APP + 0x52

    explicit KeyHook(void* target);
    ~KeyHook(); // unhooks
    KeyHook(const KeyHook&) = delete;
    KeyHook& operator=(const KeyHook&) = delete;
    KeyHook(KeyHook&&) = delete;
    KeyHook& operator=(KeyHook&&) = delete;

    // From any thread: the shortcuts from now on.
    void setKeys(Keys keys);
    // From the target's thread, once per kCommandMessage: Windows' Ctrl+Win+arrow `slides` times
    // (negative: to the left) to send while the shortcut's modifiers are still down, or 0.
    void answer(int slides) noexcept;
    [[nodiscard]] bool active() const noexcept { return m_active.load(); }

private:
    struct Access;
    struct HeldKey
    {
        std::uint32_t vk = 0;
        std::uint32_t scan = 0;
        bool extended = false;
    };
    void run();
    // Hook thread only, as the members below:
    void wait();                      // one more command to be answered
    void release();                   // the unassigned key if owed, then the modifiers held back
    void sent(std::uint32_t lastKey); // what we sent ends with the release of `lastKey` (0: nothing)
    void armTimer();

    void* m_target; // HWND
    std::atomic<std::shared_ptr<const Keys>> m_keys;
    std::array<bool, 256> m_swallowed{}; // keys whose release goes too (hook thread only)
    int m_waiting = 0;                   // commands posted, not answered yet
    bool m_maskOwed = false;        // a Win or Alt shortcut: the unassigned key goes before Win or Alt goes up
    std::vector<HeldKey> m_heldUps; // modifiers let go meanwhile, in order
    std::uint32_t m_inFlight = 0;   // the last key we sent, until it comes through the hook
    std::uintptr_t m_timer = 0;     // what never answers or never comes back does not keep them
    std::atomic<std::uint32_t> m_threadId{0};
    std::atomic<bool> m_active{false};
    std::jthread m_thread; // last
};

} // namespace rmw::tilerapp
