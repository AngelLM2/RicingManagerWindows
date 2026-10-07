#pragma once

#include "WindowOps.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace rmw::tilerapp
{

// Windows' own virtual desktops, for the Mac style (M13). What there is to read is read with the
// documented IVirtualDesktopManager (which desktop a window is on) and Explorer's registry key (the
// desktops in order, the current one); switching is Windows' own Ctrl+Win+arrows, so it slides as
// usual. Windows does not let a program move another app's window to another desktop: that stays
// with Win+Tab.
class VirtualDesktops
{
public:
    using Id = std::array<std::uint8_t, 16>; // a GUID

    struct State
    {
        std::vector<Id> desktops; // Windows' order
        int current = 0;
    };

    // Null when the interface is missing (COM must be initialized on this thread).
    [[nodiscard]] static std::unique_ptr<VirtualDesktops> create();
    ~VirtualDesktops();
    VirtualDesktops(const VirtualDesktops&) = delete;
    VirtualDesktops& operator=(const VirtualDesktops&) = delete;
    VirtualDesktops(VirtualDesktops&&) = delete;
    VirtualDesktops& operator=(VirtualDesktops&&) = delete;

    [[nodiscard]] State read() const;
    // The desktop `window` is on, as an index of `state.desktops`.
    [[nodiscard]] std::optional<int> desktopOf(Window window, const State& state) const;
    // Signaled when the desktops change (one more, one less, another current). Call rearm() after.
    [[nodiscard]] void* changed() const noexcept { return m_event; }
    void rearm() noexcept;
    // To desktop `index`, one slide at a time.
    static void goTo(int index, const State& state) noexcept;
    // The slides from the current desktop to desktop `index` (negative: to the left).
    [[nodiscard]] static int slidesTo(int index, const State& state) noexcept;
    // Ctrl+Win+arrow `times` times. The modifiers already held (the shortcut that asked, as Ctrl+Win+3)
    // are not pressed again, nor let go. The key whose release ends what was sent, or 0 for nothing.
    static std::uint32_t slide(bool next, int times) noexcept;

private:
    VirtualDesktops() = default;

    void* m_manager = nullptr; // IVirtualDesktopManager*
    void* m_key = nullptr;     // HKEY of Explorer's VirtualDesktops (or of Explorer, until it exists)
    bool m_ownKey = false;
    void* m_event = nullptr; // HANDLE
};

} // namespace rmw::tilerapp
