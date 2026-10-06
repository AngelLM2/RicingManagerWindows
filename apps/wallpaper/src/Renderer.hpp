#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/video/Plan.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rmw::wallpaper
{

// The GPU side every monitor shares: one D3D11 device (video support, multithread protected), the
// Media Foundation device manager its decoders use, and one DirectComposition device. Rebuilt as a
// whole when the GPU goes away (driver update, TDR, a GPU unplugged).
class Graphics
{
public:
    static Result<std::shared_ptr<Graphics>> create();
    ~Graphics();
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;
    Graphics(Graphics&&) = delete;
    Graphics& operator=(Graphics&&) = delete;

    struct Parts; // the COM objects (Renderer.cpp)
    [[nodiscard]] Parts& parts() const noexcept { return *m_parts; }
    [[nodiscard]] std::string adapter() const;

private:
    Graphics();
    std::unique_ptr<Parts> m_parts;
};

struct RendererOptions
{
    video::Source source = video::Source::Video;
    std::filesystem::path file;
    domain::Color color;      // Source::Color
    domain::Color background; // around a picture that does not cover the monitor
    domain::WallpaperFit fit = domain::WallpaperFit::Fill;
    video::Effects effects;
    std::vector<video::Particles> particles;
    std::uint32_t fpsLimit = 30;
    int left = 0; // the monitor, in pixels (the parallax follows the mouse on it)
    int top = 0;
    int width = 0;
    int height = 0;

    // Drawn by the Composer (else a video goes straight from the video processor to the screen).
    [[nodiscard]] bool composed() const noexcept
    {
        video::Effects rest = effects;
        rest.speed = 1.0; // the decoder's pace, not a drawing
        return source != video::Source::Video || !rest.none() || !particles.empty();
    }
    // Drawn once and left alone: a picture or a color with nothing that moves.
    [[nodiscard]] bool still() const noexcept
    {
        return source != video::Source::Video && !effects.moves() &&
               std::ranges::all_of(particles, [](const video::Particles& p) { return p.speed <= 0.0; });
    }
};

// One monitor's live wallpaper on its own thread, in a flip swap chain that DirectComposition shows
// in the window. A video decodes with Media Foundation (DXVA when the GPU can; the decoder keeps few
// frames) and scales with the D3D11 video processor, straight into the swap chain, or through the
// Composer when it has effects or particles; a picture or a color is drawn by the Composer. It
// presents only when a frame is due, and not at all while nothing changes.
class Renderer
{
public:
    Renderer(std::shared_ptr<Graphics> graphics, void* window, RendererOptions options);
    ~Renderer(); // stops the thread
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Builds everything and plays, on the renderer's own thread: opening a file and starting a
    // decoder wait on the GPU, which a game starting may keep busy for seconds, and the window
    // thread must keep answering. ready() once built; failed() when that, or playing, did not work.
    void start();
    // Paused: nothing is decoded or presented; the last frame stays on screen.
    void setPaused(bool paused);

    [[nodiscard]] bool ready() const noexcept { return m_ready.load(); }
    // Costs nothing once drawn (no frames come): kept while paused, however long.
    [[nodiscard]] bool still() const noexcept { return m_options.still(); }
    // Set by the render thread when it cannot go on (the GPU went away, the file broke).
    [[nodiscard]] bool failed() const noexcept { return m_failed.load(); }
    [[nodiscard]] bool deviceLost() const noexcept { return m_deviceLost.load(); }
    [[nodiscard]] std::string failure() const; // why, once failed()
    [[nodiscard]] std::uint64_t framesPresented() const noexcept { return m_presented.load(); }
    // "1920x1080 at 30 fps, hardware decoding, into 2560x1440", once ready().
    [[nodiscard]] std::string describe() const;

    struct Pipeline; // decoder, video processor, swap chain (Renderer.cpp)

private:
    [[nodiscard]] Result<void> build();
    [[nodiscard]] Result<void> buildVideo();
    void run(const std::stop_token& stop);
    void runVideo(const std::stop_token& stop);
    void runDrawing(const std::stop_token& stop); // a picture or a color
    void lose(long hr, std::string_view what);

    std::shared_ptr<Graphics> m_graphics;
    void* m_window; // HWND
    RendererOptions m_options;
    std::unique_ptr<Pipeline> m_pipeline;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    bool m_paused = false;
    std::string m_failure;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_deviceLost{false};
    std::atomic<std::uint64_t> m_presented{0};
    std::jthread m_thread; // last: stops before the rest goes
};

} // namespace rmw::wallpaper
