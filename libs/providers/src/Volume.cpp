#include "rmw/providers/Volume.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <wil/com.h>
#include <wil/result.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace rmw::providers
{

using namespace std::chrono_literals;

struct VolumeWatcher::Shared
{
    using Command = std::function<void(IAudioEndpointVolume&)>;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    bool alive = true;
    bool changed = true;
    std::function<void(const VolumeState&)> listener;
};

namespace
{

using Shared = VolumeWatcher::Shared;

// COM object Windows calls on its own thread: it only marks the volume as changed.
class Callback final : public IAudioEndpointVolumeCallback
{
public:
    explicit Callback(std::shared_ptr<Shared> shared)
        : m_shared(std::move(shared))
    {
    }
    virtual ~Callback() = default;
    Callback(const Callback&) = delete;
    Callback& operator=(const Callback&) = delete;
    Callback(Callback&&) = delete;
    Callback& operator=(Callback&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        if (id == __uuidof(IUnknown) || id == __uuidof(IAudioEndpointVolumeCallback))
        {
            *object = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_references;
        if (left == 0)
        {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override
    {
        {
            const std::scoped_lock lock(m_shared->mutex);
            m_shared->changed = true;
        }
        m_shared->wake.notify_all();
        return S_OK;
    }

private:
    std::atomic<ULONG> m_references{1};
    std::shared_ptr<Shared> m_shared;
};

std::pair<wil::com_ptr<IAudioEndpointVolume>, wil::com_ptr<Callback>> connect(
    const std::shared_ptr<Shared>& shared)
{
    try
    {
        auto devices = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_INPROC_SERVER);
        wil::com_ptr<IMMDevice> device;
        THROW_IF_FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put()));
        wil::com_ptr<IAudioEndpointVolume> volume;
        THROW_IF_FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                         volume.put_void()));
        wil::com_ptr<Callback> callback;
        callback.attach(new Callback(shared));
        THROW_IF_FAILED(volume->RegisterControlChangeNotify(callback.get()));
        return {volume, callback};
    }
    catch (const wil::ResultException& e)
    {
        log::warn("volume: no output device ({:#x})", static_cast<unsigned>(e.GetErrorCode()));
        return {};
    }
}

VolumeState stateOf(IAudioEndpointVolume* volume)
{
    VolumeState state;
    float level = 0.0F;
    BOOL muted = FALSE;
    if (volume != nullptr && SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)) &&
        SUCCEEDED(volume->GetMute(&muted)))
    {
        state.device = true;
        state.level = level;
        state.muted = muted != FALSE;
    }
    return state;
}

// The audio thread.
void run(const std::shared_ptr<Shared>& shared)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return;
    }
    wil::com_ptr<IAudioEndpointVolume> volume;
    wil::com_ptr<Callback> callback;
    auto lastTry = std::chrono::steady_clock::time_point{};
    while (true)
    {
        std::deque<Shared::Command> commands;
        bool changed = false;
        std::function<void(const VolumeState&)> listener;
        {
            std::unique_lock lock(shared->mutex);
            shared->wake.wait_for(
                lock, 10s, [&] { return !shared->alive || shared->changed || !shared->commands.empty(); });
            if (!shared->alive)
            {
                break;
            }
            commands.swap(shared->commands);
            changed = std::exchange(shared->changed, false);
            listener = shared->listener;
        }
        if (!volume && std::chrono::steady_clock::now() - lastTry > 10s)
        {
            // The default device may change (headphones): connect again when lost.
            lastTry = std::chrono::steady_clock::now();
            std::tie(volume, callback) = connect(shared);
            changed = true;
        }
        for (auto& command : commands)
        {
            if (volume)
            {
                command(*volume);
            }
        }
        if ((changed || !commands.empty()) && listener)
        {
            listener(stateOf(volume.get()));
        }
    }
    if (volume && callback)
    {
        volume->UnregisterControlChangeNotify(callback.get());
    }
    volume.reset();
    callback.reset();
    CoUninitialize();
}

void post(const std::shared_ptr<Shared>& shared, Shared::Command command)
{
    {
        const std::scoped_lock lock(shared->mutex);
        shared->commands.push_back(std::move(command));
    }
    shared->wake.notify_all();
}

} // namespace

VolumeWatcher::VolumeWatcher(std::function<void(const VolumeState&)> changed)
    : m_shared(std::make_shared<Shared>())
{
    m_shared->listener = std::move(changed);
    // Detached: if a call hangs, the owner must not hang waiting for it when it goes away.
    std::thread([shared = m_shared] { run(shared); }).detach();
}

VolumeWatcher::~VolumeWatcher()
{
    {
        const std::scoped_lock lock(m_shared->mutex);
        m_shared->alive = false;
        m_shared->listener = nullptr;
    }
    m_shared->wake.notify_all();
}

void VolumeWatcher::toggleMute()
{
    post(m_shared, [](IAudioEndpointVolume& volume) {
        BOOL muted = FALSE;
        if (SUCCEEDED(volume.GetMute(&muted)))
        {
            volume.SetMute(muted == FALSE ? TRUE : FALSE, nullptr);
        }
    });
}

void VolumeWatcher::step(float delta)
{
    post(m_shared, [delta](IAudioEndpointVolume& volume) {
        float level = 0.0F;
        if (SUCCEEDED(volume.GetMasterVolumeLevelScalar(&level)))
        {
            volume.SetMasterVolumeLevelScalar(std::clamp(level + delta, 0.0F, 1.0F), nullptr);
        }
    });
}

} // namespace rmw::providers
