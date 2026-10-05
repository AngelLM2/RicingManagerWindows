#include "BarApp.hpp"

#include "rmw/bar/Plan.hpp"
#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"

#include <windows.h>

#include <array>
#include <chrono>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

constexpr const wchar_t* kEventsClass = L"RicingManagerWindows.BarEvents";
constexpr UINT_PTR kRebuildTimer = 1;
constexpr UINT kRebuildDelayMs = 1500; // displays and Explorer settle first (docs/03 §9)

slint::Color colorOf(const domain::Color& color, double opacity)
{
    const auto alpha = static_cast<std::uint8_t>(std::clamp(color.a * opacity, 0.0, 255.0));
    return slint::Color::from_argb_uint8(alpha, color.r, color.g, color.b);
}

// Windows' light or dark mode: the colors a rice leaves out follow it.
bool systemLight()
{
    DWORD light = 0;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS &&
           light != 0;
}

// What the bar's own surface looks like. A solid bar is opaque unless told otherwise; transparent
// and acrylic let what is behind show unless a tint is given; mica is acrylic under a thick tint
// of the theme (DWM's own mica only shows on the focused window, and a bar never has the focus).
slint::Brush backgroundOf(const domain::BarConfig& bar, bool light)
{
    const auto& background = bar.background;
    const auto* tint = background.tint ? std::get_if<domain::Color>(&background.tint->value) : nullptr;
    const domain::Color theme =
        light ? domain::Color{0xf3, 0xf3, 0xf3, 255} : domain::Color{0x20, 0x20, 0x20, 255};
    const domain::Color base = tint != nullptr ? *tint : theme;
    double opacity = 0.0;
    switch (background.kind)
    {
    case domain::BarBackground::Kind::Solid:
        opacity = 1.0;
        break;
    case domain::BarBackground::Kind::Mica:
        opacity = 0.8;
        break;
    case domain::BarBackground::Kind::Transparent:
    case domain::BarBackground::Kind::Acrylic:
        opacity = tint != nullptr ? 0.5 : 0.0;
        break;
    }
    return {colorOf(base, background.opacity.value_or(opacity))};
}

slint::Color foregroundOf(const domain::BarConfig& bar, bool light)
{
    const auto* color = bar.foreground ? std::get_if<domain::Color>(&bar.foreground->value) : nullptr;
    if (color != nullptr)
    {
        return colorOf(*color, 1.0);
    }
    return light ? slint::Color::from_rgb_uint8(0x1b, 0x1b, 0x1b)
                 : slint::Color::from_rgb_uint8(0xf3, 0xf3, 0xf3);
}

} // namespace

struct BarApp::Access
{
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<BarApp*>( // NOLINT(performance-no-int-to-ptr)
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr)
        {
            const auto* area = reinterpret_cast<const wchar_t*>(lParam); // NOLINT(performance-no-int-to-ptr)
            const bool themeChanged =
                message == WM_SETTINGCHANGE && area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0;
            if (message == WM_DISPLAYCHANGE || themeChanged ||
                (message == self->m_taskbarCreated && self->m_taskbarCreated != 0))
            {
                self->scheduleRebuild();
                return 0;
            }
            if (message == WM_TIMER && wParam == kRebuildTimer)
            {
                KillTimer(window, kRebuildTimer);
                if (!self->rebuild())
                {
                    log::info("bar: no plan anymore; leaving");
                    slint::quit_event_loop();
                }
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

BarApp::BarApp(Win32Platform& loop, std::filesystem::path planFile)
    : m_loop(&loop)
    , m_planFile(std::move(planFile))
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Access::proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kEventsClass;
    RegisterClassExW(&windowClass);
    // Top-level (hidden): message-only windows miss WM_DISPLAYCHANGE and TaskbarCreated.
    HWND events = CreateWindowExW(WS_EX_TOOLWINDOW, kEventsClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (events != nullptr)
    {
        SetWindowLongPtrW(events, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        ChangeWindowMessageFilterEx(events, m_taskbarCreated, MSGFLT_ALLOW, nullptr);
    }
    m_events = events;
}

BarApp::~BarApp()
{
    if (m_planWatch != nullptr)
    {
        m_loop->unwatch(m_planWatch);
        FindCloseChangeNotification(m_planWatch);
    }
    clear();
    m_native.show();
    if (m_events != nullptr)
    {
        SetWindowLongPtrW(static_cast<HWND>(m_events), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_events));
    }
}

bool BarApp::start()
{
    if (!rebuild())
    {
        return false;
    }
    // The core rewrites the plan (atomically) when the rice changes: follow it.
    void* watch = FindFirstChangeNotificationW(m_planFile.parent_path().c_str(), FALSE,
                                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (watch != INVALID_HANDLE_VALUE)
    {
        m_planWatch = watch;
        m_loop->watch(watch, [this, watch] {
            FindNextChangeNotification(watch);
            scheduleRebuild();
        });
    }
    return true;
}

void BarApp::scheduleRebuild()
{
    if (m_events != nullptr)
    {
        SetTimer(static_cast<HWND>(m_events), kRebuildTimer, kRebuildDelayMs, nullptr);
    }
}

std::unique_ptr<slint::platform::WindowAdapter> BarApp::createAdapter()
{
    if (!m_pending)
    {
        // Slint asked for a window we did not plan (it should not): a hidden, harmless one.
        log::warn("bar: unexpected window request");
        return std::make_unique<BarWindow>(domain::BarConfig{},
                                           MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    }
    auto window = std::make_unique<BarWindow>(m_pending->config, m_pending->monitor);
    m_created = window.get();
    return window;
}

void BarApp::clear()
{
    // Old windows go first: their app bars give the space back before new ones ask for it.
    for (auto& shown : m_bars)
    {
        shown->providers.clear();
        shown->instance->hide();
    }
    m_bars.clear();
}

bool BarApp::rebuild()
{
    clear();
    auto bytes = platform::readFileLimited(m_planFile, limits::kManifestMaxBytes);
    if (!bytes)
    {
        log::warn("bar: no plan: {}", bytes.error().toString());
        m_native.show();
        return false;
    }
    auto plan =
        bar::planFromJson(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!plan)
    {
        log::error("bar: {}", plan.error().toString());
        m_native.show();
        return false;
    }

    if (plan->bars.empty())
    {
        m_native.show();
        return false;
    }
    platform::Win32DisplayTopology topology;
    auto monitors = topology.monitors();
    if (!monitors)
    {
        log::error("bar: monitors: {}", monitors.error().toString());
        return true; // the plan stands: try again on the next display change
    }
    std::vector<domain::MonitorFacts> facts;
    std::vector<HMONITOR> handles;
    for (const auto& monitor : *monitors)
    {
        facts.push_back(
            {monitor.identityKey, monitor.primary, monitor.left, monitor.top, monitor.right, monitor.bottom});
        const POINT center{(monitor.left + monitor.right) / 2, (monitor.top + monitor.bottom) / 2};
        handles.push_back(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST));
    }

    std::vector<void*> hideOn;
    const bool light = systemLight();
    for (const auto& config : plan->bars)
    {
        const bar::GeneratedBar generated = bar::generateBarSource(config, builtinKinds(), builtinSources());
        for (const auto& unknown : generated.unknown)
        {
            log::warn("bar: no widget '{}'; left out", unknown);
        }
        slint::interpreter::ComponentCompiler compiler;
        const auto definition = compiler.build_from_source(generated.source, "bar.slint");
        if (!definition)
        {
            for (const auto& diagnostic : compiler.diagnostics())
            {
                log::error("bar source {}:{}: {}", diagnostic.line, diagnostic.column,
                           std::string_view(diagnostic.message));
            }
            continue;
        }
        // Several bars may share a monitor; each picks its monitors on its own.
        const std::array<domain::MonitorSelector, 1> selector{config.monitor};
        const auto owners = domain::resolveMonitorRules(selector, facts);
        for (std::size_t i = 0; i < owners.size(); ++i)
        {
            if (owners[i] != 0)
            {
                continue;
            }
            m_pending = Pending{config, handles[i]};
            Instance instance = definition->create();
            m_pending.reset();
            if (m_created != nullptr)
            {
                const slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> weak(instance);
                m_created->setMatte([weak](bool white) {
                    if (auto strong = weak.lock())
                    {
                        const auto matte = white ? slint::Color::from_rgb_uint8(255, 255, 255)
                                                 : slint::Color::from_rgb_uint8(0, 0, 0);
                        (*strong)->set_property("bar-matte", Value(slint::Brush(matte)));
                    }
                });
                m_created = nullptr;
            }
            instance->set_property("bar-background", Value(backgroundOf(config, light)));
            instance->set_property("bar-radius", Value(static_cast<double>(config.radius)));
            instance->set_property("bar-foreground", Value(slint::Brush(foregroundOf(config, light))));
            auto shown = std::make_unique<Shown>(Shown{instance, {}});
            for (const auto& slot : generated.slots)
            {
                if (auto provider = makeProvider(slot, instance))
                {
                    shown->providers.push_back(std::move(provider));
                }
            }
            instance->show();
            m_bars.push_back(std::move(shown));
            if (config.hideNativeTaskbar)
            {
                hideOn.push_back(handles[i]);
            }
        }
    }
    m_native.hide(std::move(hideOn));
    log::info("bar: {} bar window(s) shown", m_bars.size());
    return true;
}

} // namespace rmw::barapp
