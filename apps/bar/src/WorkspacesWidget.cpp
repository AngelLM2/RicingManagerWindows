#include "WorkspacesWidget.hpp"

#include "SlintPlatform.hpp"

#include "rmw/tiling/Shared.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): UI thread only
std::function<void*(const slint::interpreter::ComponentInstance*)> g_barWindow;

class WorkspacesProvider;

// One per process: the tiler's event is an auto-reset one, so a single waiter hears it and tells
// every widget (a bar on each monitor has its own).
class TilerFeed
{
public:
    static TilerFeed& instance()
    {
        static TilerFeed feed;
        return feed;
    }

    void add(WorkspacesProvider* provider);
    void remove(WorkspacesProvider* provider);

    // A consistent copy of what the tiler shares; nullopt while there is no tiler.
    [[nodiscard]] std::optional<tiling::SharedState> read();

private:
    TilerFeed() = default;
    void wake();

    std::vector<WorkspacesProvider*> m_providers;
    wil::unique_event_nothrow m_changed;
    wil::unique_handle m_mapping;
    const tiling::SharedState* m_view = nullptr;
};

class WorkspacesProvider final : public Provider
{
public:
    WorkspacesProvider(const bar::Slot& slot, const Instance& instance)
        : m_property(slot.prefix + "-desktops")
        , m_instance(instance)
        , m_raw(instance.operator->())
    {
        instance->set_callback(slot.prefix + "-select", [this](std::span<const Value> arguments) {
            if (const auto index = arguments.empty() ? std::nullopt : arguments[0].to_number())
            {
                go(static_cast<int>(*index));
            }
            return Value();
        });
        TilerFeed::instance().add(this);
        // The bar's window exists once all its widgets are made: the first look comes just after.
        m_first.start(slint::TimerMode::SingleShot, std::chrono::milliseconds(0), [this] { refresh(); });
    }

    ~WorkspacesProvider() override { TilerFeed::instance().remove(this); }
    WorkspacesProvider(const WorkspacesProvider&) = delete;
    WorkspacesProvider& operator=(const WorkspacesProvider&) = delete;
    WorkspacesProvider(WorkspacesProvider&&) = delete;
    WorkspacesProvider& operator=(WorkspacesProvider&&) = delete;

    void refresh()
    {
        auto model = std::make_shared<slint::VectorModel<Value>>();
        const auto state = TilerFeed::instance().read();
        if (const auto bounds = monitorBounds(); state && bounds && state->paused == 0)
        {
            const auto count = std::min<std::uint32_t>(state->monitorCount, tiling::kMaxSharedMonitors);
            for (std::uint32_t m = 0; m < count; ++m)
            {
                const tiling::SharedMonitor& monitor = state->monitors.at(m);
                if (monitor.left != bounds->left || monitor.top != bounds->top || monitor.right != bounds->right ||
                    monitor.bottom != bounds->bottom)
                {
                    continue;
                }
                // The ones with windows and the one shown, in order.
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(state->workspaces, 32); ++i)
                {
                    const bool active = i == monitor.active;
                    if (active || (monitor.occupied & (1U << i)) != 0)
                    {
                        model->push_back(Value(slint::interpreter::Struct{
                            {"index", Value(static_cast<double>(i))}, {"active", Value(active)}}));
                    }
                }
                break;
            }
        }
        if (auto strong = m_instance.lock())
        {
            (*strong)->set_property(m_property, Value(std::shared_ptr<slint::Model<Value>>(model)));
        }
    }

private:
    [[nodiscard]] std::optional<RECT> monitorBounds() const
    {
        HWND window = g_barWindow ? static_cast<HWND>(g_barWindow(m_raw)) : nullptr;
        if (window == nullptr)
        {
            return std::nullopt;
        }
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) == FALSE)
        {
            return std::nullopt;
        }
        return info.rcMonitor;
    }

    void go(int index) const
    {
        HWND control = FindWindowW(tiling::kControlClass, nullptr);
        const auto bounds = monitorBounds();
        if (control == nullptr || !bounds || index < 0 || index > 31)
        {
            return;
        }
        // A point on this monitor tells the tiler which monitor's workspace.
        const auto x = static_cast<std::int16_t>((bounds->left + bounds->right) / 2);
        const auto y = static_cast<std::int16_t>((bounds->top + bounds->bottom) / 2);
        PostMessageW(control, tiling::kGoToMessage, static_cast<WPARAM>(index) + 1,
                     MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
    }

    std::string m_property;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    const slint::interpreter::ComponentInstance* m_raw;
    slint::Timer m_first;
};

void TilerFeed::add(WorkspacesProvider* provider)
{
    if (!m_changed)
    {
        // Made here too when the tiler is not running yet: it opens the same one when it starts.
        m_changed.reset(CreateEventW(nullptr, FALSE, FALSE, tiling::kStateEventName));
        if (auto* loop = Win32Platform::current(); loop != nullptr && m_changed)
        {
            loop->watch(m_changed.get(), [this] { wake(); });
        }
    }
    m_providers.push_back(provider);
}

void TilerFeed::remove(WorkspacesProvider* provider)
{
    std::erase(m_providers, provider);
    if (m_providers.empty() && m_changed)
    {
        if (auto* loop = Win32Platform::current(); loop != nullptr)
        {
            loop->unwatch(m_changed.get());
        }
        m_changed.reset();
    }
}

void TilerFeed::wake()
{
    const auto providers = m_providers; // a refresh may rebuild nothing, but keep it safe
    for (WorkspacesProvider* provider : providers)
    {
        provider->refresh();
    }
}

std::optional<tiling::SharedState> TilerFeed::read()
{
    if (m_view == nullptr)
    {
        m_mapping.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, tiling::kStateMappingName));
        if (!m_mapping)
        {
            return std::nullopt;
        }
        m_view = static_cast<const tiling::SharedState*>(
            MapViewOfFile(m_mapping.get(), FILE_MAP_READ, 0, 0, sizeof(tiling::SharedState)));
        if (m_view == nullptr)
        {
            m_mapping.reset();
            return std::nullopt;
        }
    }
    const auto* sequence = reinterpret_cast<const volatile LONG*>(&m_view->sequence);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const LONG before = *sequence;
        if ((before & 1) != 0)
        {
            YieldProcessor(); // being written
            continue;
        }
        tiling::SharedState copy;
        std::memcpy(&copy, m_view, sizeof(copy));
        MemoryBarrier();
        if (*sequence == before)
        {
            return copy.version == tiling::kSharedVersion ? std::optional(copy) : std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

std::unique_ptr<Provider> makeWorkspacesProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<WorkspacesProvider>(slot, instance);
}

void setBarWindowLookup(std::function<void*(const slint::interpreter::ComponentInstance*)> lookup)
{
    g_barWindow = std::move(lookup);
}

} // namespace rmw::barapp
