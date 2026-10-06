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
class KeyHook
{
public:
    using Keys = std::vector<domain::TilingKey>;
    // To the target window: wParam = the domain::TilingCommand, lParam = its number (1..9) or 0.
    static constexpr std::uint32_t kCommandMessage = 0x8000 + 0x52; // WM_APP + 0x52

    explicit KeyHook(void* target);
    ~KeyHook(); // unhooks
    KeyHook(const KeyHook&) = delete;
    KeyHook& operator=(const KeyHook&) = delete;
    KeyHook(KeyHook&&) = delete;
    KeyHook& operator=(KeyHook&&) = delete;

    // From any thread: the shortcuts from now on.
    void setKeys(Keys keys);
    [[nodiscard]] bool active() const noexcept { return m_active.load(); }

private:
    struct Access;
    void run();

    void* m_target; // HWND
    std::atomic<std::shared_ptr<const Keys>> m_keys;
    std::array<bool, 256> m_swallowed{}; // keys whose release goes too (hook thread only)
    std::atomic<std::uint32_t> m_threadId{0};
    std::atomic<bool> m_active{false};
    std::jthread m_thread; // last
};

} // namespace rmw::tilerapp
