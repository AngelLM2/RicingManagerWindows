#pragma once

#include <slint.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace rmw::settingsapp
{

// Thumbnails of images, videos, folders and cursors, as Explorer shows them (docs/07: Windows' own
// thumbnail cache, which already knows videos), made on a thread of their own: the window never
// waits for a file. The UI process is the only one that reads the user's images (the core never
// decodes images).
class Thumbnails
{
public:
    Thumbnails();
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;
    Thumbnails(Thumbnails&&) = delete;
    Thumbnails& operator=(Thumbnails&&) = delete;

    using Ready = std::function<void(const slint::Image&)>;
    // `ready` runs on the UI thread, at once when cached. A file that has no thumbnail never calls.
    void request(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height, Ready ready);

private:
    struct Job
    {
        std::filesystem::path file;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        Ready ready;
    };
    using Key = std::pair<std::wstring, std::uint32_t>;

    void work(const std::stop_token& stop);

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Job> m_jobs;
    std::map<Key, slint::Image> m_cache; // UI thread only
    std::shared_ptr<std::atomic<bool>> m_done =
        std::make_shared<std::atomic<bool>>(false); // the thread ended
    std::jthread m_thread;                          // last: stops first
};

} // namespace rmw::settingsapp
