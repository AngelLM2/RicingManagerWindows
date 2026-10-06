#include "SystemWidgets.hpp"

#include "SlintPlatform.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/providers/System.hpp"
#include "rmw/providers/Volume.hpp"

#include <windows.h>

#include <shellapi.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
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

// A Segoe Fluent Icons glyph (Private Use Area, so three bytes of UTF-8).
std::string glyph(char32_t code)
{
    return {static_cast<char>(0xE0U | ((code >> 12U) & 0x0FU)),
            static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)), static_cast<char>(0x80U | (code & 0x3FU))};
}

std::string rate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024.0 * 1024.0)
    {
        return std::format("{:.1f} MB/s", bytesPerSecond / (1024.0 * 1024.0));
    }
    return std::format("{:.0f} KB/s", bytesPerSecond / 1024.0);
}

// A status that refreshes itself: CPU, memory, network, battery. An icon, and its text when the
// rice's `label` says so.
class LabelProvider : public Provider
{
public:
    LabelProvider(const bar::Slot& slot, const Instance& instance, std::chrono::milliseconds period,
                  bool textByDefault)
        : m_prefix(slot.prefix)
        , m_instance(instance)
    {
        instance->set_property(m_prefix + "-show-text",
                               Value(settingBool(*slot.item, "label", textByDefault)));
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

    void showGlyph(const std::string& icon)
    {
        if (icon == m_glyph)
        {
            return;
        }
        m_glyph = icon;
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-glyph", Value(slint::SharedString(icon)));
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
    std::string m_glyph;
    bool m_shown = true;
    slint::Timer m_timer; // last: stops before the rest goes away
};

class CpuProvider final : public LabelProvider
{
public:
    CpuProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 2s, true)
        , m_format(settingString(*slot.item, "format", "{}%"))
    {
        showGlyph(glyph(0xE9D9)); // Diagnostic
        refresh();
    }

private:
    void refresh() override
    {
        if (const auto load = m_sampler.sample())
        {
            show(fill(m_format, std::format("{:.0f}", *load)));
        }
    }

    std::string m_format;
    providers::CpuSampler m_sampler;
};

class MemoryProvider final : public LabelProvider
{
public:
    MemoryProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 5s, true)
        , m_format(settingString(*slot.item, "format", "{}%"))
    {
        showGlyph(glyph(0xE9F5)); // Processing
        refresh();
    }

private:
    void refresh() override
    {
        if (const auto memory = providers::sampleMemory())
        {
            show(fill(m_format, std::to_string(memory->load)));
        }
    }

    std::string m_format;
};

class NetworkProvider final : public LabelProvider
{
public:
    NetworkProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 2s, true)
        , m_format(settingString(*slot.item, "format", "↓ {} ↑ {}"))
    {
        showGlyph(glyph(0xE774)); // Globe
        refresh();
    }

private:
    void refresh() override
    {
        const auto rates = m_sampler.sample();
        if (!rates)
        {
            return;
        }
        const std::string down = rate(rates->downBytesPerSecond);
        const std::string up = rate(rates->upBytesPerSecond);
        // Two placeholders: down, then up.
        const auto first = m_format.find("{}");
        show(first == std::string::npos
                 ? m_format
                 : fill(m_format.substr(0, first + 2), down) + fill(m_format.substr(first + 2), up));
    }

    std::string m_format;
    providers::NetworkSampler m_sampler;
};

class BatteryProvider final : public LabelProvider
{
public:
    BatteryProvider(const bar::Slot& slot, const Instance& instance)
        : LabelProvider(slot, instance, 30s, true)
        , m_format(settingString(*slot.item, "format", "{}%"))
    {
        refresh();
    }

private:
    void refresh() override
    {
        const auto battery = providers::sampleBattery();
        if (!battery.present)
        {
            show({}, false); // a desktop: nothing to show
            return;
        }
        // Battery0..10 and BatteryCharging0..10: tenths of a charge.
        constexpr std::array<char32_t, 11> kLevels = {0xE850, 0xE851, 0xE852, 0xE853, 0xE854, 0xE855,
                                                      0xE856, 0xE857, 0xE858, 0xE859, 0xE83F};
        constexpr std::array<char32_t, 11> kCharging = {0xE85A, 0xE85B, 0xE85C, 0xE85D, 0xE85E, 0xE85F,
                                                        0xE860, 0xE861, 0xE862, 0xE83E, 0xEA93};
        const auto tenth =
            static_cast<std::size_t>(std::clamp((static_cast<int>(battery.percent) + 5) / 10, 0, 10));
        showGlyph(glyph(battery.pluggedIn ? kCharging[tenth] : kLevels[tenth]));
        show(fill(m_format, std::to_string(battery.percent)));
    }

    std::string m_format;
};

// The default output device's volume (providers::VolumeWatcher talks to Core Audio on a thread of
// its own). Only text crosses back to the UI thread.
class VolumeProvider final : public Provider
{
public:
    VolumeProvider(const bar::Slot& slot, const Instance& instance)
        : m_prefix(slot.prefix)
        , m_instance(instance)
        , m_owner(std::make_shared<Owner>())
        , m_watcher([owner = m_owner](const providers::VolumeState& state) { publish(owner, state); })
    {
        m_owner->provider = this;
        instance->set_property(m_prefix + "-show-text", Value(settingBool(*slot.item, "label", false)));
        instance->set_callback(m_prefix + "-clicked", [this](std::span<const Value>) {
            m_watcher.toggleMute();
            return Value();
        });
        instance->set_callback(m_prefix + "-scrolled", [this](std::span<const Value> arguments) {
            if (const auto notches = arguments.empty() ? std::nullopt : arguments[0].to_number())
            {
                m_watcher.step(static_cast<float>(*notches) * 0.02F);
            }
            return Value();
        });
        show({});
    }

    ~VolumeProvider() override
    {
        const std::scoped_lock lock(m_owner->mutex);
        m_owner->provider = nullptr;
    }
    VolumeProvider(const VolumeProvider&) = delete;
    VolumeProvider& operator=(const VolumeProvider&) = delete;
    VolumeProvider(VolumeProvider&&) = delete;
    VolumeProvider& operator=(VolumeProvider&&) = delete;

private:
    struct Owner
    {
        std::mutex mutex;
        VolumeProvider* provider = nullptr; // UI thread only, and only while it lives
    };

    // Hands the state to the UI thread; Slint objects are touched only there.
    static void publish(const std::shared_ptr<Owner>& owner, providers::VolumeState state)
    {
        slint::invoke_from_event_loop([owner, state] {
            VolumeProvider* provider = nullptr;
            {
                const std::scoped_lock lock(owner->mutex);
                provider = owner->provider;
            }
            if (provider != nullptr)
            {
                provider->show(state);
            }
        });
    }

    void show(const std::optional<providers::VolumeState>& state)
    {
        // No device yet: the speaker alone. Muted, then Volume0..3 by level.
        char32_t icon = 0xE767;
        std::string text;
        if (state && state->device)
        {
            const int percent = static_cast<int>(std::lround(state->level * 100.0F));
            icon = state->muted   ? 0xE74F
                   : percent == 0 ? 0xE992
                   : percent < 34 ? 0xE993
                   : percent < 67 ? 0xE994
                                  : 0xE995;
            text = state->muted ? std::string() : std::format("{}%", percent);
        }
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_prefix + "-glyph", Value(slint::SharedString(glyph(icon))));
            (*instance)->set_property(m_prefix + "-text", Value(slint::SharedString(text)));
            (*instance)->set_property(m_prefix + "-shown", Value(true));
        }
    }

    std::string m_prefix;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    std::shared_ptr<Owner> m_owner;
    providers::VolumeWatcher m_watcher; // last: stops before the rest goes away
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

    void refresh()
    {
        const auto desktops = providers::readVirtualDesktops();
        m_current = desktops.current;
        m_count = desktops.count;
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
