#include "Thumbnails.hpp"

#include <windows.h>

#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace rmw::settingsapp
{
namespace
{

constexpr std::size_t kCacheMax = 256;
constexpr auto kStopPatience = std::chrono::seconds(1);

// The bitmap's pixels, top-down RGBA. Thumbnails of photos come without alpha (all zero): opaque.
std::optional<slint::SharedPixelBuffer<slint::Rgba8Pixel>> pixelsOf(HBITMAP bitmap)
{
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0)
    {
        return std::nullopt;
    }
    const auto width = static_cast<std::uint32_t>(info.bmWidth);
    const auto height = static_cast<std::uint32_t>(info.bmHeight);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(header.bmiHeader);
    header.bmiHeader.biWidth = info.bmWidth;
    header.bmiHeader.biHeight = -info.bmHeight; // top-down
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4);
    const wil::unique_hdc_window screen(GetDC(nullptr));
    if (GetDIBits(screen.get(), bitmap, 0, height, bgra.data(), &header, DIB_RGB_COLORS) == 0)
    {
        return std::nullopt;
    }
    bool anyAlpha = false;
    for (std::size_t i = 3; i < bgra.size() && !anyAlpha; i += 4)
    {
        anyAlpha = bgra[i] != 0;
    }
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(width, height);
    auto* out = buffer.begin();
    for (std::size_t i = 0; i < bgra.size(); i += 4, ++out)
    {
        // Shell bitmaps carry premultiplied alpha; Slint takes it straight.
        const std::uint8_t a = anyAlpha ? bgra[i + 3] : 255;
        const auto unpremultiply = [a](std::uint8_t c) {
            return a == 0 || a == 255 ? c : static_cast<std::uint8_t>(std::min(255, c * 255 / a));
        };
        *out = {unpremultiply(bgra[i + 2]), unpremultiply(bgra[i + 1]), unpremultiply(bgra[i]), a};
    }
    return buffer;
}

} // namespace

Thumbnails::Thumbnails()
    : m_thread([this, done = m_done](const std::stop_token& stop) {
        work(stop);
        *done = true;
    })
{
}

Thumbnails::~Thumbnails()
{
    m_thread.request_stop();
    m_wake.notify_all();
    // The shell can take as long as it likes over one file (a video, a folder on the network): a closing
    // window does not wait for it. Once stopped, the thread touches nothing of this object (its job is
    // its own), so it may finish after it.
    const auto deadline = std::chrono::steady_clock::now() + kStopPatience;
    while (!*m_done && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (*m_done)
    {
        m_thread.join();
    }
    else
    {
        m_thread.detach();
    }
}

void Thumbnails::request(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                         Ready ready)
{
    if (file.empty())
    {
        return;
    }
    const Key key{file.native(), (width << 16) | height};
    if (const auto found = m_cache.find(key); found != m_cache.end())
    {
        ready(found->second);
        return;
    }
    // The answer comes back to this thread, which keeps it.
    // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
    Ready store = [this, key, ready = std::move(ready)](const slint::Image& image) {
        if (m_cache.size() >= kCacheMax)
        {
            m_cache.clear();
        }
        m_cache[key] = image;
        ready(image);
    };
    {
        const std::scoped_lock lock(m_mutex);
        m_jobs.push_back({file, width, height, std::move(store)});
    }
    m_wake.notify_one();
}

void Thumbnails::work(const std::stop_token& stop)
{
    const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);
    while (!stop.stop_requested())
    {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [this] { return !m_jobs.empty(); }))
            {
                return;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        wil::com_ptr_nothrow<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(job.file.c_str(), nullptr, IID_PPV_ARGS(factory.put()))))
        {
            continue;
        }
        wil::unique_hbitmap bitmap;
        const SIZE size{static_cast<LONG>(job.width), static_cast<LONG>(job.height)};
        if (FAILED(factory->GetImage(size, SIIGBF_BIGGERSIZEOK, bitmap.put())))
        {
            continue;
        }
        auto pixels = pixelsOf(bitmap.get());
        if (!pixels)
        {
            continue;
        }
        // Built on the UI thread: Slint images belong to it.
        slint::invoke_from_event_loop(
            [pixels = std::move(*pixels), ready = std::move(job.ready)]() { ready(slint::Image(pixels)); });
    }
}

} // namespace rmw::settingsapp
