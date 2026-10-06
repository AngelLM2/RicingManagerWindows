#pragma once

#include "Desktop.hpp"
#include "Renderer.hpp"

#include "rmw/platform/Supervisor.hpp"
#include "rmw/video/Plan.hpp"
#include "rmw/video/Playback.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace rmw::wallpaper
{

// rmw-wallpaper (docs/04 M6): plays the plan's videos behind the desktop icons, one window per
// monitor, and pauses them when nobody would see them (a fullscreen app, a covered desktop, the
// battery, a locked session, the display off). Follows the plan file (rewritten: it rebuilds;
// gone: it leaves), the monitors, Explorer restarts and the GPU going away. A supervised worker:
// the core restarts it when it crashes or hangs.
//
// The window thread only handles windows and messages; whatever waits on the GPU (making the
// device, opening a file, starting a decoder) runs on other threads, so a busy GPU (a game
// starting) is never taken for a hang.
class WallpaperApp
{
public:
    WallpaperApp(std::filesystem::path planFile, platform::Heartbeat heartbeat);
    ~WallpaperApp();
    WallpaperApp(const WallpaperApp&) = delete;
    WallpaperApp& operator=(const WallpaperApp&) = delete;
    WallpaperApp(WallpaperApp&&) = delete;
    WallpaperApp& operator=(WallpaperApp&&) = delete;

    // Until the plan goes away (or has no video anymore). The exit code.
    int run();

    struct Access;      // the window procedure
    struct GraphicsJob; // the GPU device, being made on its own thread

private:
    struct Screen
    {
        std::string monitor; // identity key
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        void* window = nullptr; // HWND
        Attachment attachment;
        std::unique_ptr<Renderer> renderer;
        bool announced = false; // its renderer is built, and that was logged
        bool shown = false;     // its window is visible (from the first frame on)
        video::PauseReason paused = video::PauseReason::None;
        std::optional<video::Clock::time_point> pausedSince;
        std::size_t entry = 0;
        std::string video; // what plays: size, rate, decoding
        std::string error; // why it does not play (until the next rebuild)
        bool noDesktop = false; // it could not go behind the icons: Explorer was not (yet) there
    };

    [[nodiscard]] bool loadPlan();
    void rebuild();
    void clear();
    void startGraphics(); // the device, on another thread; rebuild() again when it is there
    void onGraphics();
    [[nodiscard]] bool buildScreen(Screen& screen);
    void release(Screen& screen);
    // Renderers that got ready, showed a first frame or failed. True while one is still starting.
    [[nodiscard]] bool watchRenderers();
    void tick(); // once a second: conditions, attachment, failures
    void updatePauses();
    // <data>\wallpaper\status.json, for `rmw status` and the settings: what each monitor plays,
    // why it is paused, what went wrong. Rewritten only when it changes.
    void writeStatus();
    void onMessage(unsigned message, std::uintptr_t wParam, std::intptr_t lParam);
    // Logs what the window thread is busy with when it takes over a second: a hang the core ends
    // is easier to fix with a name.
    void watchWindowThread(const std::stop_token& stop);

    std::filesystem::path m_planFile;
    platform::Heartbeat m_heartbeat;
    std::optional<video::Clock::time_point> m_graphicsFailedAt; // retried later
    std::shared_ptr<GraphicsJob> m_graphicsJob;
    video::Plan m_plan;
    std::shared_ptr<Graphics> m_graphics;
    std::vector<Screen> m_screens;
    std::string m_error; // why nothing plays at all (no GPU, no desktop)
    std::string m_lastStatus;
    video::SessionState m_session;
    void* m_events = nullptr; // HWND: broadcasts, power, session
    unsigned m_taskbarCreated = 0;
    void* m_displayNotification = nullptr;
    bool m_rebuildPending = false;
    // A monitor found no desktop to go behind (Explorer restarting, or not started yet): tried
    // again a little later, even if Explorer's "taskbar created" never reaches us.
    std::optional<video::Clock::time_point> m_desktopRetryAt;
    bool m_waitingForDesktop = false;
    bool m_quit = false;
    int m_exitCode = 0;
    std::atomic<const char*> m_busyWith{nullptr}; // what the window thread does now
    std::atomic<video::Clock::rep> m_busySince{0};
    std::jthread m_watch; // last: stops first
};

} // namespace rmw::wallpaper
