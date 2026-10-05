#pragma once

#include "rmw/base/Error.hpp"

#include <string>

namespace rmw::wallpaper
{

// Where a video window lives: behind the desktop icons, in front of Windows' own wallpaper
// (docs/09 §1, ADR-0006). Two layouts:
// - "raised" (Windows 11 24H2+): the icons (SHELLDLL_DefView) are a child of Progman, which has
//   WS_EX_NOREDIRECTIONBITMAP; our window becomes a layered child of Progman right below them;
// - legacy: Progman spawns a WorkerW behind the icons' window (message 0x052C); ours goes in it.
// Nothing of ours runs in Explorer: these are our windows, parented to Explorer's.
struct Attachment
{
    void* parent = nullptr; // HWND
    bool raised = false;
};

// Attaches `window` (HWND) over the monitor rectangle (virtual-screen pixels).
[[nodiscard]] Result<Attachment> attachBehindIcons(void* window, int left, int top, int width, int height);
// Whether `window` is still where attachBehindIcons put it (Explorer restarts, theme changes and
// virtual desktops may recreate those windows).
[[nodiscard]] bool stillAttached(void* window, const Attachment& attachment);

} // namespace rmw::wallpaper
