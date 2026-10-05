#include "SystemWidgets.hpp"

#include "SlintPlatform.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"

// The network tables need Winsock's headers before <windows.h>: keep this order.
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
// clang-format on

#include <shellapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;
using namespace std::chrono_literals;

// "{}" in a rice-given format is the value; nothing else is interpreted (a rice is untrusted).
std::string fill(std::string_view format, std::string_view value)
{
    std::string text;
    for (std::size_t i = 0; i < format.size() && text.size() < 128; ++i)
    {
        if (format.substr(i, 2) == "{}")
        {
            text += value;
            ++i;
            continue;
        }
        text += format[i];
    }
    return text;
}

std::string rate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024.0 * 1024.0)
    {
        return std::format("{:.1f} MB/s", bytesPerSecond / (1024.0 * 1024.0));
    }
    return std::format("{:.0f} KB/s", bytesPerSecond / 1024.0);
}

std::uint64_t ticks(const FILETIME& time) noexcept
{
    return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}

// A text widget that refreshes itself: CPU, memory, network, battery.
class LabelProvider : public Provider
{
public:
    LabelProvider(const bar::Slot& slot, const Instance& instance, std::chrono::milliseconds period)
        : m_prefix(slot.prefix)
        , m_instance(instance)
    {
        m_timer.start(slint::TimerMode::Repeated, period, [this] { refresh(); });
    }

protected:
    void show(const std::string& text, bool shown = true)
    {
        if (text == m_text && shown == m_shown)
        {
            return;
        }
        m_text = text;
        m_shown = shown;
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-text", Value(slint::SharedString(text)));
            (*instance)->set_property(m_prefix + "-shown", Value(shown));
        }
    }

    // Called by the derived constructor once, then on the timer.
    virtual void refresh() = 0;

    [[nodiscard]] const std::string& prefix() const noexcept { return m_prefix; }
    [[nodiscard]] const slint::ComponentWeakHandle<slint::interpreter::ComponentInstance>& instance()
        const noexcept
    {
        return m_instance;
    }

private:
    std::string m_prefix;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    std::string m_text;
    bool m_shown = true;
    slint::Timer m_timer; // last: stops before the rest goes away
};

class CpuProvider final : public LabelProvider
{
public:
    CpuProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 2s)
        , m_format(settingString(*slot.item, "format", "CPU {}%"))
    {
        refresh();
    }

private:
    void refresh() override
    {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetSystemTimes(&idle, &kernel, &user) == FALSE)
        {
            return;
        }
        const std::uint64_t idleNow = ticks(idle);
        const std::uint64_t busyNow = ticks(kernel) + ticks(user); // kernel time includes idle
        if (m_busy != 0 && busyNow > m_busy)
        {
            const auto total = static_cast<double>(busyNow - m_busy);
            const double load = 1.0 - (static_cast<double>(idleNow - m_idle) / total);
            show(fill(m_format, std::format("{:.0f}", std::clamp(load, 0.0, 1.0) * 100.0)));
        }
        m_idle = idleNow;
        m_busy = busyNow;
    }

    std::string m_format;
    std::uint64_t m_idle = 0;
    std::uint64_t m_busy = 0;
};

class MemoryProvider final : public LabelProvider
{
public:
    MemoryProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 5s)
        , m_format(settingString(*slot.item, "format", "RAM {}%"))
    {
        refresh();
    }

private:
    void refresh() override
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) != FALSE)
        {
            show(fill(m_format, std::to_string(status.dwMemoryLoad)));
        }
    }

    std::string m_format;
};

class NetworkProvider final : public LabelProvider
{
public:
    NetworkProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 2s)
        , m_format(settingString(*slot.item, "format", "↓ {} ↑ {}"))
    {
        refresh();
    }

private:
    void refresh() override
    {
        MIB_IF_TABLE2* table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
        {
            return;
        }
        std::uint64_t in = 0;
        std::uint64_t out = 0;
        const std::span rows(static_cast<const MIB_IF_ROW2*>(table->Table), table->NumEntries);
        for (const auto& row : rows)
        {
            // Real adapters that are up: no loopback, no filter and virtual-switch duplicates.
            if (row.OperStatus == IfOperStatusUp &&
                row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE &&
                row.Type != IF_TYPE_SOFTWARE_LOOPBACK)
            {
                in += row.InOctets;
                out += row.OutOctets;
            }
        }
        FreeMibTable(table);
        const auto now = std::chrono::steady_clock::now();
        if (m_seen)
        {
            const double seconds = std::chrono::duration<double>(now - m_at).count();
            if (seconds > 0.1 && in >= m_in && out >= m_out)
            {
                const std::string down = rate(static_cast<double>(in - m_in) / seconds);
                const std::string up = rate(static_cast<double>(out - m_out) / seconds);
                // Two placeholders: down, then up.
                const auto first = m_format.find("{}");
                show(first == std::string::npos
                         ? m_format
                         : fill(m_format.substr(0, first + 2), down) + fill(m_format.substr(first + 2), up));
            }
        }
        m_seen = true;
        m_in = in;
        m_out = out;
        m_at = now;
    }

    std::string m_format;
    bool m_seen = false;
    std::uint64_t m_in = 0;
    std::uint64_t m_out = 0;
    std::chrono::steady_clock::time_point m_at;
};

class BatteryProvider final : public LabelProvider
{
public:
    BatteryProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 30s)
        , m_format(settingString(*slot.item, "format", "{}%"))
    {
        refresh();
    }

private:
    void refresh() override
    {
        SYSTEM_POWER_STATUS power{};
        constexpr BYTE kNoBattery = 128;
        constexpr BYTE kUnknown = 255;
        if (GetSystemPowerStatus(&power) == FALSE || power.BatteryFlag == kNoBattery ||
            power.BatteryFlag == kUnknown || power.BatteryLifePercent > 100)
        {
            show({}, false); // a desktop: nothing to show
            return;
        }
        std::string text = fill(m_format, std::to_string(power.BatteryLifePercent));
        if (power.ACLineStatus == 1)
        {
            text += " +"; // charging
        }
        show(text);
    }

    std::string m_format;
};

// The default output device's volume. Core Audio calls go to a Windows service and may take
// seconds (it starts late, a device changes): they all run on a thread of their own, never on the
// UI thread, whose heartbeat the supervisor watches. Only text crosses back to the UI.
class VolumeProvider final : public Provider
{
public:
    VolumeProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_instance(instance)
        , m_shared(std::make_shared<Shared>())
    {
        m_shared->owner = this;
        instance->set_callback(m_prefix + "-clicked", [shared = m_shared](std::span<const Value>) {
            post(shared, [](IAudioEndpointVolume& volume) {
                BOOL muted = FALSE;
                if (SUCCEEDED(volume.GetMute(&muted)))
                {
                    volume.SetMute(muted == FALSE ? TRUE : FALSE, nullptr);
                }
            });
            return Value();
        });
        instance->set_callback(m_prefix + "-scrolled", [shared = m_shared](std::span<const Value> arguments) {
            const auto notches = arguments.empty() ? std::nullopt : arguments[0].to_number();
            if (notches)
            {
                const auto step = static_cast<float>(*notches) * 0.02F;
                post(shared, [step](IAudioEndpointVolume& volume) {
                    float level = 0.0F;
                    if (SUCCEEDED(volume.GetMasterVolumeLevelScalar(&level)))
                    {
                        volume.SetMasterVolumeLevelScalar(std::clamp(level + step, 0.0F, 1.0F), nullptr);
                    }
                });
            }
            return Value();
        });
        show("—");
        // Detached: if a call hangs, the bar must not hang waiting for it when it closes.
        std::thread([shared = m_shared] { run(shared); }).detach();
    }

    ~VolumeProvider() override
    {
        {
            const std::scoped_lock lock(m_shared->mutex);
            m_shared->alive = false;
            m_shared->owner = nullptr;
        }
        m_shared->wake.notify_all();
    }
    VolumeProvider(const VolumeProvider&) = delete;
    VolumeProvider& operator=(const VolumeProvider&) = delete;
    VolumeProvider(VolumeProvider&&) = delete;
    VolumeProvider& operator=(VolumeProvider&&) = delete;

private:
    using Command = std::function<void(IAudioEndpointVolume&)>;

    struct Shared
    {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Command> commands;
        bool alive = true;
        bool changed = true;
        VolumeProvider* owner = nullptr; // UI thread only, and only while alive
    };

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

    static void post(const std::shared_ptr<Shared>& shared, Command command)
    {
        {
            const std::scoped_lock lock(shared->mutex);
            shared->commands.push_back(std::move(command));
        }
        shared->wake.notify_all();
    }

    // The audio thread.
    static void run(const std::shared_ptr<Shared>& shared)
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
            std::deque<Command> commands;
            bool changed = false;
            {
                std::unique_lock lock(shared->mutex);
                shared->wake.wait_for(lock, 10s, [&] {
                    return !shared->alive || shared->changed || !shared->commands.empty();
                });
                if (!shared->alive)
                {
                    break;
                }
                commands.swap(shared->commands);
                changed = std::exchange(shared->changed, false);
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
            if (changed || !commands.empty())
            {
                publish(shared, textOf(volume.get()));
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

    static std::pair<wil::com_ptr<IAudioEndpointVolume>, wil::com_ptr<Callback>> connect(
        const std::shared_ptr<Shared>& shared)
    {
        try
        {
            auto devices =
                wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>(CLSCTX_INPROC_SERVER);
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

    static std::string textOf(IAudioEndpointVolume* volume)
    {
        float level = 0.0F;
        BOOL muted = FALSE;
        if (volume == nullptr || FAILED(volume->GetMasterVolumeLevelScalar(&level)) ||
            FAILED(volume->GetMute(&muted)))
        {
            return "—"; // no device
        }
        return muted != FALSE ? std::string("Mute")
                              : std::format("Vol {}%", static_cast<int>(std::lround(level * 100.0F)));
    }

    // Hands the text to the UI thread; Slint objects are touched only there.
    static void publish(const std::shared_ptr<Shared>& shared, std::string text)
    {
        slint::invoke_from_event_loop([shared, text = std::move(text)] {
            VolumeProvider* owner = nullptr;
            {
                const std::scoped_lock lock(shared->mutex);
                owner = shared->alive ? shared->owner : nullptr;
            }
            if (owner != nullptr)
            {
                owner->show(text);
            }
        });
    }

    void show(const std::string& text)
    {
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-text", Value(slint::SharedString(text)));
            (*instance)->set_property(m_prefix + "-shown", Value(true));
        }
    }

    std::string m_prefix;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    std::shared_ptr<Shared> m_shared;
};

class NotificationsProvider final : public Provider
{
public:
    NotificationsProvider(const bar::Slot& slot, const Instance& instance)
    {
        const std::string target = settingString(*slot.item, "opens", "notifications");
        const wchar_t* uri = target == "quick-settings" ? L"ms-actioncenter:controlcenter/&showFooter=true"
                                                        : L"ms-actioncenter:";
        instance->set_callback(slot.prefix + "-clicked", [uri](std::span<const Value>) {
            ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
            return Value();
        });
    }
};

// Virtual desktops: Explorer keeps their list and the current one in the registry, which tells
// us when it changes. Switching is the keyboard shortcut (there is no public API for it).
class DesktopsProvider final : public Provider
{
public:
    DesktopsProvider(const bar::Slot& slot, const Instance& instance)
        : m_property(slot.prefix + "-desktops")
        , m_instance(instance)
    {
        instance->set_callback(slot.prefix + "-switch", [this](std::span<const Value> arguments) {
            const auto index = arguments.empty() ? std::nullopt : arguments[0].to_number();
            if (index)
            {
                go(static_cast<int>(*index));
            }
            return Value();
        });
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops)", 0,
                          KEY_READ | KEY_NOTIFY, m_key.put()) == ERROR_SUCCESS)
        {
            m_changed.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            arm();
            if (auto* loop = Win32Platform::current(); loop != nullptr && m_changed)
            {
                loop->watch(m_changed.get(), [this] {
                    arm();
                    refresh();
                });
            }
        }
        refresh();
    }

    ~DesktopsProvider() override
    {
        if (auto* loop = Win32Platform::current(); loop != nullptr && m_changed)
        {
            loop->unwatch(m_changed.get());
        }
    }
    DesktopsProvider(const DesktopsProvider&) = delete;
    DesktopsProvider& operator=(const DesktopsProvider&) = delete;
    DesktopsProvider(DesktopsProvider&&) = delete;
    DesktopsProvider& operator=(DesktopsProvider&&) = delete;

private:
    void arm()
    {
        if (m_key && m_changed)
        {
            RegNotifyChangeKeyValue(m_key.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, m_changed.get(), TRUE);
        }
    }

    [[nodiscard]] std::vector<GUID> readIds(const wchar_t* value) const
    {
        std::vector<GUID> ids;
        DWORD bytes = 0;
        if (!m_key ||
            RegQueryValueExW(m_key.get(), value, nullptr, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes % sizeof(GUID) != 0 || bytes > 64 * sizeof(GUID))
        {
            return ids;
        }
        ids.resize(bytes / sizeof(GUID));
        if (RegQueryValueExW(m_key.get(), value, nullptr, nullptr, reinterpret_cast<BYTE*>(ids.data()),
                             &bytes) != ERROR_SUCCESS)
        {
            ids.clear();
        }
        return ids;
    }

    void refresh()
    {
        auto ids = readIds(L"VirtualDesktopIDs");
        const auto current = readIds(L"CurrentVirtualDesktop");
        if (ids.empty())
        {
            ids.resize(1); // never switched desktops yet: there is one
        }
        m_current = 0;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (!current.empty() && IsEqualGUID(ids[i], current.front()) != FALSE)
            {
                m_current = static_cast<int>(i);
            }
        }
        m_count = static_cast<int>(ids.size());
        auto model = std::make_shared<slint::VectorModel<Value>>();
        for (int i = 0; i < m_count; ++i)
        {
            model->push_back(Value(slint::interpreter::Struct{{"index", Value(static_cast<double>(i))},
                                                              {"active", Value(i == m_current)}}));
        }
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_property, Value(std::shared_ptr<slint::Model<Value>>(model)));
        }
    }

    void go(int index) const
    {
        const int steps = index - m_current;
        if (steps == 0 || index < 0 || index >= m_count)
        {
            return;
        }
        const WORD arrow = steps > 0 ? VK_RIGHT : VK_LEFT;
        std::vector<INPUT> keys;
        const auto key = [&](WORD vk, bool up) {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = vk;
            input.ki.dwFlags =
                (up ? KEYEVENTF_KEYUP : 0U) | (vk == VK_LEFT || vk == VK_RIGHT ? KEYEVENTF_EXTENDEDKEY : 0U);
            keys.push_back(input);
        };
        key(VK_CONTROL, false);
        key(VK_LWIN, false);
        for (int i = 0; i < std::abs(steps); ++i)
        {
            key(arrow, false);
            key(arrow, true);
        }
        key(VK_LWIN, true);
        key(VK_CONTROL, true);
        SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
    }

    std::string m_property;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    wil::unique_hkey m_key;
    wil::unique_event_nothrow m_changed;
    int m_current = 0;
    int m_count = 1;
};

} // namespace

std::unique_ptr<Provider> makeSystemProvider(const bar::Slot& slot, const Instance& instance)
{
    const std::string_view id = slot.kind->id;
    if (id == "cpu")
    {
        return std::make_unique<CpuProvider>(slot, instance);
    }
    if (id == "memory")
    {
        return std::make_unique<MemoryProvider>(slot, instance);
    }
    if (id == "network")
    {
        return std::make_unique<NetworkProvider>(slot, instance);
    }
    if (id == "battery")
    {
        return std::make_unique<BatteryProvider>(slot, instance);
    }
    if (id == "volume")
    {
        return std::make_unique<VolumeProvider>(slot, instance);
    }
    if (id == "notifications")
    {
        return std::make_unique<NotificationsProvider>(slot, instance);
    }
    if (id == "desktops")
    {
        return std::make_unique<DesktopsProvider>(slot, instance);
    }
    return nullptr;
}

} // namespace rmw::barapp
