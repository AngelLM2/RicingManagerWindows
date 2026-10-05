#include "TrayForwarder.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/NativeTaskbar.hpp"

#include <windows.h>

#include <algorithm>
#include <optional>

namespace rmw::barapp
{
namespace
{

using namespace std::chrono_literals;
namespace tray = bar::tray;

constexpr UINT kTimeoutMs = 5000; // nobody waits for this thread: Explorer may take its time
constexpr auto kRetryEvery = 2s;
constexpr int kMaxAttempts = 60; // two minutes

HICON iconOf(std::uint32_t handle) noexcept
{
    return reinterpret_cast<HICON>(
        static_cast<std::intptr_t>(static_cast<std::int32_t>(handle))); // NOLINT(performance-no-int-to-ptr)
}

// 32 bits on the wire: a 64-bit process truncates its handles (and sign-extends them back).
std::uint32_t wireOf(HICON icon) noexcept
{
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(icon));
}

} // namespace

TrayForwarder::TrayForwarder()
    : m_thread([this](const std::stop_token& stop) { run(stop); })
{
}

TrayForwarder::~TrayForwarder()
{
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    for (auto& [key, job] : m_queue)
    {
        release(job);
    }
    for (auto& [key, pending] : m_pending)
    {
        for (auto& job : pending.jobs)
        {
            release(job);
        }
    }
}

void TrayForwarder::send(std::vector<std::byte> payload, std::uintptr_t wParam, const tray::Update& update)
{
    Job job;
    job.payload = std::move(payload);
    job.wParam = wParam;
    job.command = update.command;
    if ((update.flags & tray::kFlagIcon) != 0 && update.icon != 0)
    {
        // Now, while the app waits for us: it may destroy its icon as soon as we answer.
        if (HICON copy = CopyIcon(iconOf(update.icon)))
        {
            job.icon = copy;
            tray::setIcon(job.payload, wireOf(copy));
        }
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_queue.emplace_back(tray::identity(update), std::move(job));
    }
    m_wake.notify_one();
}

void TrayForwarder::release(Job& job) noexcept
{
    if (job.icon != nullptr)
    {
        DestroyIcon(static_cast<HICON>(job.icon));
        job.icon = nullptr;
    }
}

bool TrayForwarder::deliver(Job& job)
{
    auto* const explorer = static_cast<HWND>(platform::nativeTrayWindow());
    if (explorer == nullptr)
    {
        return false; // restarting: later
    }
    const auto send = [&](tray::Command command) -> std::optional<LRESULT> {
        tray::setCommand(job.payload, command);
        COPYDATASTRUCT data{};
        data.dwData = tray::kNotifyIconData;
        data.cbData = static_cast<DWORD>(job.payload.size());
        data.lpData = job.payload.data();
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(explorer, WM_COPYDATA, job.wParam, reinterpret_cast<LPARAM>(&data),
                                SMTO_ABORTIFHUNG, kTimeoutMs, &result) == 0)
        {
            return std::nullopt;
        }
        return static_cast<LRESULT>(result);
    };
    const auto answer = send(job.command);
    if (!answer)
    {
        return false; // hung or slow: later
    }
    if (*answer != FALSE || job.command == tray::Command::Delete)
    {
        return true; // done (deleting an icon Explorer does not have is done too)
    }
    if (job.command == tray::Command::Add)
    {
        // It has the icon already (we asked the app to add it again): an update is what is meant.
        const auto updated = send(tray::Command::Modify);
        tray::setCommand(job.payload, tray::Command::Add); // a later retry adds again
        if (updated && *updated != FALSE)
        {
            return true;
        }
    }
    return false;
}

void TrayForwarder::run(const std::stop_token& stop)
{
    while (!stop.stop_requested())
    {
        std::deque<std::pair<std::string, Job>> incoming;
        {
            std::unique_lock lock(m_mutex);
            const auto ready = [this] { return !m_queue.empty(); };
            if (m_pending.empty())
            {
                m_wake.wait(lock, stop, ready);
            }
            else
            {
                auto due = m_pending.begin()->second.due;
                for (const auto& [key, pending] : m_pending)
                {
                    due = std::min(due, pending.due);
                }
                m_wake.wait_until(lock, stop, due, ready);
            }
            incoming.swap(m_queue);
        }
        if (stop.stop_requested())
        {
            for (auto& [key, job] : incoming)
            {
                release(job);
            }
            break;
        }
        const auto now = Clock::now();
        for (auto& [key, job] : incoming)
        {
            if (job.command == tray::Command::Delete)
            {
                // What was waiting for this icon no longer matters.
                if (const auto found = m_pending.find(key); found != m_pending.end())
                {
                    for (auto& old : found->second.jobs)
                    {
                        release(old);
                    }
                    m_pending.erase(found);
                }
            }
            if (const auto found = m_pending.find(key); found != m_pending.end())
            {
                found->second.jobs.push_back(std::move(job)); // after what Explorer refused
                continue;
            }
            if (deliver(job))
            {
                release(job);
                continue;
            }
            m_pending[key].jobs.push_back(std::move(job));
            m_pending[key].due = now + kRetryEvery;
        }
        // Again, what is due.
        for (auto it = m_pending.begin(); it != m_pending.end();)
        {
            Pending& pending = it->second;
            if (pending.due > Clock::now())
            {
                ++it;
                continue;
            }
            while (!pending.jobs.empty() && deliver(pending.jobs.front()))
            {
                release(pending.jobs.front());
                pending.jobs.pop_front();
            }
            if (!pending.jobs.empty() && ++pending.jobs.front().attempts >= kMaxAttempts)
            {
                log::warn("tray: Explorer keeps refusing icon {}; giving up on it (the bar still shows it)",
                          it->first);
                for (auto& job : pending.jobs)
                {
                    release(job);
                }
                pending.jobs.clear();
            }
            if (pending.jobs.empty())
            {
                it = m_pending.erase(it);
            }
            else
            {
                pending.due = Clock::now() + kRetryEvery;
                ++it;
            }
        }
    }
}

} // namespace rmw::barapp
