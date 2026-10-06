#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace rmw::barapp
{

// What the Win key did, as the hook sees it (pure: testable without a keyboard).
class WinKeyTracker
{
public:
    enum class Verdict : std::uint8_t
    {
        Pass,     // let it through untouched
        OpenOurs, // Win pressed and released alone: swallow its release, open our Start
    };
    // A key went down or up (not one the hook sent itself).
    [[nodiscard]] Verdict key(std::uint32_t virtualKey, bool down) noexcept;

private:
    bool m_winDown = false;
    bool m_combined = false; // another key while Win was down: a Windows shortcut (Win+E, Win+L...)
};

// The Win key alone opens Ricing Manager's Start (M11, opt-in): a low-level keyboard hook on a
// thread of its own (like PowerToys), so a busy bar never slows typing nor gets the hook dropped.
// Nothing is blocked but the release of a lone Win: the hook sends an unassigned key first (what
// AutoHotkey calls the menu mask key) and then that release, so Windows sees a Win "shortcut" and
// keeps its own Start closed. Every Win+key shortcut, Win+L and Ctrl+Alt+Del stay Windows'. The
// hook goes with the process: if rmw-bar ends, the Win key is Windows' again.
class WinKeyHook
{
public:
    // `open` runs on the hook's thread: post it to the UI thread.
    explicit WinKeyHook(std::function<void()> open);
    ~WinKeyHook(); // unhooks
    WinKeyHook(const WinKeyHook&) = delete;
    WinKeyHook& operator=(const WinKeyHook&) = delete;
    WinKeyHook(WinKeyHook&&) = delete;
    WinKeyHook& operator=(WinKeyHook&&) = delete;

    [[nodiscard]] bool active() const noexcept { return m_active.load(); }

private:
    struct Access;
    void run();

    std::function<void()> m_open;
    WinKeyTracker m_tracker;
    std::atomic<std::uint32_t> m_threadId{0};
    std::atomic<bool> m_active{false};
    std::jthread m_thread; // last
};

} // namespace rmw::barapp
