#pragma once

#include <vector>

// Windows' own taskbar while our bar replaces it (ADR-0003 decision 3). Its reserved space goes
// away with auto-hide, which the core applies through the engine (so it is undone like any other
// change); here it is only hidden and shown, which Windows forgets when Explorer restarts.
namespace rmw::platform
{

// Shows every native taskbar again. Never blocks: Explorer may be hung (ShowWindowAsync).
void showNativeTaskbars() noexcept;

// Keeps the taskbars of some monitors hidden: hides them now and again whenever Explorer shows
// them (it does after a restart, a DPI or a display change). One per process; on the thread that
// pumps messages (the hook is out-of-context: no code goes into Explorer).
class NativeTaskbarHider
{
public:
    NativeTaskbarHider() = default;
    ~NativeTaskbarHider(); // shows them back
    NativeTaskbarHider(const NativeTaskbarHider&) = delete;
    NativeTaskbarHider& operator=(const NativeTaskbarHider&) = delete;
    NativeTaskbarHider(NativeTaskbarHider&&) = delete;
    NativeTaskbarHider& operator=(NativeTaskbarHider&&) = delete;

    // HMONITORs whose taskbar goes away; empty shows everything back.
    void hide(std::vector<void*> monitors);
    void show() noexcept;

private:
    struct Access;
    void rehide(void* window) noexcept;

    std::vector<void*> m_monitors;
    std::vector<void*> m_hidden; // HWNDs
    void* m_hook = nullptr;      // HWINEVENTHOOK
};

} // namespace rmw::platform
