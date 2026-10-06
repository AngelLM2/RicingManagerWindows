#include "BarApp.hpp"

#include "TrayHost.hpp"
#include "WindowList.hpp"

#include "rmw/bar/Plan.hpp"
#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Pipe.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/providers/Windows.hpp"

#include <windows.h>

#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

constexpr const wchar_t* kEventsClass = L"RicingManagerWindows.BarEvents";
constexpr UINT_PTR kRebuildTimer = 1;
constexpr UINT kRebuildDelayMs = 1500; // displays and Explorer settle first (docs/03 §9)
constexpr UINT kReloadDelayMs = 150;   // a saved plugin file: editors write in bursts
constexpr std::size_t kMaxDevWatches = 16;

// A callback's arguments as the plugin host gets them.
domain::Value::Array argumentsOf(std::span<const Value> arguments)
{
    domain::Value::Array list;
    for (const auto& argument : arguments)
    {
        if (const auto number = argument.to_number())
        {
            list.push_back(domain::Value{*number});
        }
        else if (const auto text = argument.to_string())
        {
            list.push_back(domain::Value{std::string(*text)});
        }
        else if (const auto flag = argument.to_bool())
        {
            list.push_back(domain::Value{*flag});
        }
        else
        {
            list.emplace_back();
        }
    }
    return list;
}

// Whether a value names a window's icon somewhere (it must follow the window list).
bool hasWindowImage(const domain::Value& value)
{
    if (const auto* text = std::get_if<std::string>(&value.data))
    {
        return text->starts_with("window:");
    }
    if (const auto* items = std::get_if<domain::Value::Array>(&value.data))
    {
        return std::ranges::any_of(*items, hasWindowImage);
    }
    if (const auto* members = value.table())
    {
        return std::ranges::any_of(*members,
                                   [](const domain::ValueMember& m) { return hasWindowImage(m.value); });
    }
    return false;
}

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

// Windows' accent color (the rice sets it), lighter on a dark bar, as Windows draws it there.
slint::Color accentOf(bool light)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\DWM)", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    {
        value = 0xFFD47800; // Windows' default blue (ABGR)
    }
    const auto channel = [&](unsigned shift) {
        const auto c = static_cast<double>((value >> shift) & 0xFFU);
        return static_cast<std::uint8_t>(light ? c : c + ((255.0 - c) * 0.35));
    };
    return slint::Color::from_rgb_uint8(channel(0), channel(8), channel(16));
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

BarApp::BarApp(Win32Platform& loop, std::filesystem::path planFile, std::wstring bridgePipe)
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
    if (!bridgePipe.empty())
    {
        auto self = platform::currentExecutable();
        const auto host = self ? self->parent_path() / L"rmw-plugin-host.exe" : std::filesystem::path();
        m_bridge = std::make_unique<PluginBridge>(
            std::move(bridgePipe), host,
            [this](bar::bridge::Message message) { onPluginMessage(std::move(message)); },
            [this] {
                m_bridge->send(bar::bridge::Hello{});
                sendInstances();
            });
    }
}

BarApp::~BarApp()
{
    try
    {
        m_bridge.reset(); // joins its threads
        watchTray(false);
        watchDevPlugins({});
        if (m_windowSubscription != 0)
        {
            WindowTracker::instance().unsubscribe(m_windowSubscription);
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): the native taskbar must still come back below
    {
    }
    if (m_planWatch != nullptr)
    {
        m_loop->unwatch(m_planWatch);
        FindCloseChangeNotification(m_planWatch);
    }
    clear();
    TrayHost::instance().shutdown(); // apps keep their icons in Explorer's tray
    m_native.show();
    if (m_events != nullptr)
    {
        SetWindowLongPtrW(static_cast<HWND>(m_events), GWLP_USERDATA, 0);
        DestroyWindow(static_cast<HWND>(m_events));
    }
}

bool BarApp::start()
{
    // The core rewrites the plan (atomically) when the rice changes: follow it. Watched before the
    // first read, so a plan written in between is seen.
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
    return rebuild();
}

void BarApp::scheduleRebuild()
{
    if (m_events != nullptr)
    {
        SetTimer(static_cast<HWND>(m_events), kRebuildTimer, kRebuildDelayMs, nullptr);
    }
}

void BarApp::scheduleReload()
{
    if (m_events != nullptr)
    {
        SetTimer(static_cast<HWND>(m_events), kRebuildTimer, kReloadDelayMs, nullptr);
    }
}

void BarApp::watchDevPlugins(const std::vector<bar::PlanPlugin>& plugins)
{
    std::vector<std::filesystem::path> wanted;
    for (const auto& plugin : plugins)
    {
        if (plugin.dev && wanted.size() < kMaxDevWatches)
        {
            wanted.push_back(plugin.folder);
        }
    }
    std::erase_if(m_devWatches, [&](const auto& watch) {
        if (std::ranges::find(wanted, watch.first) != wanted.end())
        {
            return false;
        }
        m_loop->unwatch(watch.second);
        FindCloseChangeNotification(watch.second);
        return true;
    });
    for (const auto& folder : wanted)
    {
        if (std::ranges::any_of(m_devWatches, [&](const auto& watch) { return watch.first == folder; }))
        {
            continue;
        }
        void* watch = FindFirstChangeNotificationW(
            folder.c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (watch == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        m_devWatches.emplace_back(folder, watch);
        m_loop->watch(watch, [this, watch] {
            FindNextChangeNotification(watch);
            scheduleReload();
        });
    }
}

// Tries each plugin widget alone in `config`'s bar. The ones Slint refuses are left out, with
// Slint's messages pointed at their own files; true when one was.
bool BarApp::leaveOutBrokenWidgets(const domain::BarConfig& config)
{
    std::vector<std::string> broken;
    for (const auto& widget : m_plugins.all())
    {
        std::vector<bar::WidgetKind> kinds(builtinKinds().begin(), builtinKinds().end());
        kinds.push_back(widget->kind);
        const bar::GeneratedBar generated =
            bar::generateBarSource(config, kinds, PluginWidgets::sourcesWith(*widget, builtinSources()));
        slint::interpreter::ComponentCompiler compiler;
        if (compiler.build_from_source(generated.source, "bar.slint"))
        {
            continue;
        }
        const std::size_t before = PluginWidgets::lineBefore(generated.source, widget->id);
        const auto lines = static_cast<std::size_t>(std::ranges::count(widget->checked.source, '\n'));
        for (const auto& diagnostic : compiler.diagnostics())
        {
            if (diagnostic.level != slint::interpreter::DiagnosticLevel::Error)
            {
                continue;
            }
            const std::size_t line = diagnostic.line;
            if (before != 0 && line > before && line <= before + lines)
            {
                log::error("bar: plugin widget {}: {}:{}:{}: {}", widget->id, widget->manifest.ui,
                           line - before, diagnostic.column, std::string_view(diagnostic.message));
            }
            else
            {
                // In the bar around it: the component does not fit what plugin.toml declares.
                log::error("bar: plugin widget {}: does not fit plugin.toml: {}", widget->id,
                           std::string_view(diagnostic.message));
            }
        }
        log::error("bar: plugin widget {} left out: Slint refuses it", widget->id);
        broken.push_back(widget->id);
    }
    for (const auto& id : broken)
    {
        m_plugins.remove(id);
    }
    return !broken.empty();
}

std::unique_ptr<slint::platform::WindowAdapter> BarApp::createAdapter()
{
    if (startMenuWantsWindow())
    {
        return makeStartMenuWindow();
    }
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

void BarApp::openStart(bool fromKey)
{
    if (!m_start)
    {
        return;
    }
    // The bar the click came from (under the mouse); for the Win key, the main monitor's.
    POINT cursor{};
    GetCursorPos(&cursor);
    const Shown* chosen = nullptr;
    const auto hasStart = [](const domain::BarConfig& config) {
        return std::ranges::any_of(std::array{&config.left, &config.center, &config.right}, [](const auto* zone) {
            return std::ranges::any_of(*zone, [](const auto& item) { return item.id == "start"; });
        });
    };
    for (const auto& shown : m_bars)
    {
        RECT rect{};
        if (!fromKey && shown->window != nullptr &&
            GetWindowRect(static_cast<HWND>(shown->window->hwnd()), &rect) != FALSE && PtInRect(&rect, cursor) != FALSE)
        {
            chosen = shown.get();
            break;
        }
    }
    if (chosen == nullptr)
    {
        auto* const primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        for (const auto& shown : m_bars)
        {
            if (shown->window != nullptr && hasStart(shown->config) &&
                MonitorFromWindow(static_cast<HWND>(shown->window->hwnd()), MONITOR_DEFAULTTONEAREST) == primary)
            {
                chosen = shown.get();
                break;
            }
        }
    }
    if (chosen == nullptr && !m_bars.empty())
    {
        chosen = m_bars.front().get();
    }
    if (chosen == nullptr || chosen->window == nullptr)
    {
        return;
    }
    const auto hwnd = static_cast<HWND>(chosen->window->hwnd());
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    auto* const monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    StartAnchor anchor;
    anchor.bar = {rect.left, rect.top, rect.right, rect.bottom};
    anchor.work = {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    anchor.scale = dpiX / 96.0;
    anchor.edge = chosen->config.position;
    anchor.centered = std::ranges::any_of(chosen->config.center, [](const auto& item) { return item.id == "start"; });
    m_start->toggle(anchor);
}

void BarApp::clear()
{
    m_pluginSlots.clear();
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
    m_plugins.load(plan->plugins);
    watchDevPlugins(plan->plugins);
    auto pluginKinds = m_plugins.kinds();
    std::string pluginSources = m_plugins.sources(builtinSources());
    for (const auto& config : plan->bars)
    {
        bar::GeneratedBar generated = bar::generateBarSource(config, pluginKinds, pluginSources);
        slint::interpreter::ComponentCompiler compiler;
        auto definition = compiler.build_from_source(generated.source, "bar.slint");
        // A plugin widget the checks let through but Slint refuses must not take the bar down:
        // the ones Slint refuses alone are left out, and the rest shows.
        if (!definition && !m_plugins.all().empty() && leaveOutBrokenWidgets(config))
        {
            pluginKinds = m_plugins.kinds();
            pluginSources = m_plugins.sources(builtinSources());
            generated = bar::generateBarSource(config, pluginKinds, pluginSources);
            definition = compiler.build_from_source(generated.source, "bar.slint");
        }
        if (!definition && !m_plugins.all().empty())
        {
            // Only together do they fail: the bar shows without any.
            for (const auto& diagnostic : compiler.diagnostics())
            {
                log::error("bar: with plugin widgets {}:{}: {}", diagnostic.line, diagnostic.column,
                           std::string_view(diagnostic.message));
            }
            log::error("bar: the plugin widgets do not compile; showing the bar without them");
            generated = bar::generateBarSource(config, builtinKinds(), builtinSources());
            definition = compiler.build_from_source(generated.source, "bar.slint");
        }
        for (const auto& unknown : generated.unknown)
        {
            log::warn("bar: no widget '{}'; left out", unknown);
        }
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
            BarWindow* const created = m_created;
            if (m_created != nullptr)
            {
                const slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> weak(instance);
                m_created->setContentLength([weak] {
                    if (auto strong = weak.lock())
                    {
                        if (auto value = (*strong)->get_property("content-length"))
                        {
                            return value->to_number().value_or(0.0);
                        }
                    }
                    return 0.0;
                });
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
            instance->set_property("bar-accent", Value(slint::Brush(accentOf(light))));
            auto shown = std::make_unique<Shown>(Shown{instance, {}, created, config});
            for (const auto& slot : generated.slots)
            {
                if (const PluginWidget* widget = m_plugins.find(slot.kind->id))
                {
                    PluginSlot plugin{std::format("{}/{}", m_bars.size(), slot.prefix), widget, slot.prefix,
                                      instance, slot.item->config};
                    for (const auto& callback : widget->manifest.callbacks)
                    {
                        instance->set_callback(
                            std::format("{}-{}", slot.prefix, callback.name),
                            [this, key = plugin.key, name = callback.name](std::span<const Value> args) {
                                if (m_bridge)
                                {
                                    m_bridge->send(bar::bridge::Event{key, name, argumentsOf(args)});
                                }
                                return Value();
                            });
                    }
                    if (const auto cached = m_pluginValues.find(plugin.key);
                        cached != m_pluginValues.end() && cached->second.plugin == widget->id)
                    {
                        applyProperties(plugin, cached->second.values);
                    }
                    m_pluginSlots.push_back(std::move(plugin));
                    continue;
                }
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

    // Ricing Manager's Start (M11): made when the plan asks for it (again when it changes), in the
    // first bar's colors; the Win key opens it too when the user wants that.
    if (plan->start.ours)
    {
        if (!m_start || plan->start != m_startPlan)
        {
            m_start.reset();
            m_start = StartMenu::create(plan->start);
        }
        if (m_start)
        {
            const auto& first = plan->bars.front();
            const auto* tint = first.background.tint ? std::get_if<domain::Color>(&first.background.tint->value) : nullptr;
            const domain::Color base = tint != nullptr ? *tint
                                       : light ? domain::Color{0xf3, 0xf3, 0xf3, 255}
                                               : domain::Color{0x20, 0x20, 0x20, 255};
            m_start->setLook(colorOf(base, 0.96), foregroundOf(first, light), accentOf(light),
                             first.radius > 0 ? std::min(first.radius, 16U) : 8.0);
        }
    }
    else
    {
        m_start.reset();
    }
    m_startPlan = plan->start;
    setStartOpener(m_start ? std::function<bool()>([this] {
                       openStart(false);
                       return true;
                   })
                           : std::function<bool()>());
    if (m_start && plan->start.winKey)
    {
        if (!m_winKey)
        {
            m_winKey = std::make_unique<WinKeyHook>([this] { slint::invoke_from_event_loop([this] { openStart(true); }); });
        }
    }
    else
    {
        m_winKey.reset();
    }
    // Values of widgets that are gone (or whose place another plugin took) go with them.
    std::erase_if(m_pluginValues, [this](const auto& entry) {
        return std::ranges::none_of(m_pluginSlots, [&](const PluginSlot& slot) {
            return slot.key == entry.first && slot.widget->id == entry.second.plugin;
        });
    });
    if (!m_pluginSlots.empty() && m_windowSubscription == 0)
    {
        // Window icons a plugin shows load late: show them when they come.
        m_windowSubscription = WindowTracker::instance().subscribe([this] {
            for (const auto& slot : m_pluginSlots)
            {
                const auto cached = m_pluginValues.find(slot.key);
                if (cached != m_pluginValues.end() && hasWindowImage(domain::Value{cached->second.values}))
                {
                    applyProperties(slot, cached->second.values);
                }
            }
        });
    }
    sendInstances();
    log::info("bar: {} bar window(s) shown", m_bars.size());
    return true;
}

void BarApp::sendInstances()
{
    if (!m_bridge)
    {
        return;
    }
    bar::bridge::Instances instances;
    for (const auto& slot : m_pluginSlots)
    {
        instances.list.push_back({slot.key, slot.widget->id, slot.config});
    }
    m_bridge->send(instances);
}

void BarApp::onPluginMessage(bar::bridge::Message message)
{
    if (const auto* set = std::get_if<bar::bridge::Set>(&message))
    {
        const auto slot = std::ranges::find(m_pluginSlots, set->key, &PluginSlot::key);
        if (slot == m_pluginSlots.end() || set->properties.table() == nullptr)
        {
            return; // a widget the bar no longer shows
        }
        auto& entry = m_pluginValues[slot->key];
        if (entry.plugin != slot->widget->id)
        {
            log::info("bar: first values for {} ({})", slot->key, slot->widget->id);
            entry = {slot->widget->id, {}};
        }
        auto& cached = entry.values;
        for (const auto& member : *set->properties.table())
        {
            const auto found = std::ranges::find(cached, member.key, &domain::ValueMember::key);
            if (found != cached.end())
            {
                found->value = member.value;
            }
            else
            {
                cached.push_back(member);
            }
        }
        applyProperties(*slot, *set->properties.table());
    }
    else if (const auto* action = std::get_if<bar::bridge::Action>(&message))
    {
        if (action->name == "tray.click")
        {
            trayAction(*action);
        }
        else
        {
            windowAction(*action);
        }
    }
    else if (const auto* watch = std::get_if<bar::bridge::Watch>(&message))
    {
        watchTray(std::ranges::find(watch->providers, "tray") != watch->providers.end());
    }
    else if (const auto* hello = std::get_if<bar::bridge::Hello>(&message))
    {
        if (hello->version != bar::bridge::kVersion)
        {
            log::error("bar: the plugin host speaks bridge version {} (we speak {})", hello->version,
                       bar::bridge::kVersion);
        }
    }
}

void BarApp::applyProperties(const PluginSlot& slot, const domain::Value::Table& properties)
{
    auto instance = slot.instance.lock();
    if (!instance)
    {
        return;
    }
    for (const auto& member : properties)
    {
        const auto& declared = slot.widget->manifest.properties;
        const auto property = std::ranges::find(declared, member.key, &domain::WidgetProperty::name);
        if (property == declared.end())
        {
            log::warn("bar: {} has no property '{}'", slot.widget->id, member.key);
            continue;
        }
        auto value = m_plugins.convert(*slot.widget, property->type, member.value);
        if (!value)
        {
            log::warn("bar: {}.{}: {}", slot.widget->id, member.key, value.error().message());
            continue;
        }
        if (!(*instance)->set_property(std::format("{}-{}", slot.prefix, member.key), *value))
        {
            log::warn("bar: {}.{} was refused by the widget", slot.widget->id, member.key);
        }
    }
}

void BarApp::watchTray(bool wanted)
{
    if (wanted == (m_traySubscription != 0))
    {
        return;
    }
    if (!wanted)
    {
        TrayHost::instance().unsubscribe(m_traySubscription);
        m_traySubscription = 0;
        return;
    }
    log::info("bar: plugins watch the tray");
    m_traySubscription = TrayHost::instance().subscribe([this] { provideTray(); });
    provideTray();
}

// What plugins see of the tray (provider "tray", sensitive: tips may say what the user does).
void BarApp::provideTray()
{
    if (!m_bridge)
    {
        return;
    }
    domain::Value::Array list;
    for (const auto& view : TrayHost::instance().icons())
    {
        if (view.icon.hidden)
        {
            continue;
        }
        list.push_back(domain::Value{domain::Value::Table{
            {"id", domain::Value{static_cast<std::int64_t>(view.icon.number)}},
            {"tip", domain::Value{view.tip}},
            {"app", domain::Value{view.app}},
            {"promoted", domain::Value{view.promoted}},
        }});
    }
    m_bridge->send(bar::bridge::Provide{"tray", domain::Value{std::move(list)}});
}

// tray.click { id = <icon>, button = "left" | "right" | "double" | "middle" }: as if the user
// clicked it in our bar (the plugin's widget got the click).
void BarApp::trayAction(const bar::bridge::Action& action)
{
    const auto* id = action.arguments.find("id");
    const auto* number = id != nullptr ? std::get_if<std::int64_t>(&id->data) : nullptr;
    const auto* buttonValue = action.arguments.find("button");
    const auto* button = buttonValue != nullptr ? std::get_if<std::string>(&buttonValue->data) : nullptr;
    const std::string which = button != nullptr ? *button : "left";
    std::optional<bar::tray::Gesture> gesture;
    if (which == "left")
    {
        gesture = bar::tray::Gesture::LeftClick;
    }
    else if (which == "right")
    {
        gesture = bar::tray::Gesture::RightClick;
    }
    else if (which == "double")
    {
        gesture = bar::tray::Gesture::DoubleClick;
    }
    else if (which == "middle")
    {
        gesture = bar::tray::Gesture::MiddleClick;
    }
    if (number == nullptr || *number <= 0 || !gesture ||
        TrayHost::instance().find(static_cast<std::uint64_t>(*number)) == nullptr)
    {
        log::warn("bar: tray.click: no such icon or button");
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    TrayHost::instance().gesture(static_cast<std::uint64_t>(*number), *gesture, cursor.x, cursor.y);
}

void BarApp::windowAction(const bar::bridge::Action& action)
{
    const auto* id = action.arguments.find("id");
    std::uint64_t handle = 0;
    if (id != nullptr)
    {
        if (const auto* number = std::get_if<std::int64_t>(&id->data); number != nullptr && *number > 0)
        {
            handle = static_cast<std::uint64_t>(*number);
        }
    }
    auto* window =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)); // NOLINT(performance-no-int-to-ptr)
    // Only windows a taskbar would show: a plugin cannot reach anything else.
    if (window == nullptr || !providers::isTaskbarWindow(window))
    {
        log::warn("bar: {}: not a window of the taskbar", action.name);
        return;
    }
    if (action.name == "window.activate")
    {
        providers::activateWindow(window); // the foreground now, not what the plugin last heard
    }
    else if (action.name == "window.focus")
    {
        providers::focusWindow(window);
    }
    else if (action.name == "window.minimize")
    {
        providers::minimizeWindow(window);
    }
    else if (action.name == "window.close")
    {
        providers::closeWindow(window);
    }
}

} // namespace rmw::barapp
