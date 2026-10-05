#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
    std::filesystem::path file;
    domain::WallpaperFit fit = domain::WallpaperFit::Fill;
    std::uint32_t fpsLimit = 30;
    int width = 0; // the monitor, in pixels
    int height = 0;
};

// One monitor's video: decodes with Media Foundation (DXVA when the GPU can; the decoder keeps few
// frames), scales with the D3D11 video processor straight into a flip swap chain that
// DirectComposition shows in the window. Its own thread; it presents only when a frame is due.
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
    void run(const std::stop_token& stop);

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
