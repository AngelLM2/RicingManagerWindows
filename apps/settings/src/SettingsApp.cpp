#include "SettingsApp.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/domain/Color.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/package/Package.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/plugins/HostPlan.hpp"
#include "rmw/plugins/PluginFiles.hpp"
#include "rmw/settings/WallpaperEngine.hpp"
#include "rmw/video/Effects.hpp"

#include <windows.h>

#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <format>
#include <map>
#include <utility>

namespace rmw::settingsapp
{
namespace
{

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using ipc::Json;

constexpr std::uint32_t kPreviewWidth = 320;
constexpr std::uint32_t kPreviewHeight = 180;
constexpr std::size_t kProblemsShown = 40;
constexpr auto kRefreshEvery = 3s;
constexpr auto kBarSaveDelay = 400ms;

constexpr auto kSelectors =
    std::to_array<std::string_view>({"all", "primary", "left", "right", "top", "bottom"});
constexpr auto kBuiltinActions = std::to_array<std::string_view>(
    {"rice.apply", "rice.undo", "restore.original", "appearance.toggle", "appearance.dark",
     "appearance.light", "bar.toggle", "bar.on", "bar.off", "wallpaper.next", "launch.app"});
constexpr auto kCursorSizes = std::to_array<std::uint32_t>({0, 32, 48, 64, 96, 128});

std::string utf8(const fs::path& path)
{
    return utf16To8Lossy(path.native());
}

fs::path pathOf(const slint::SharedString& text)
{
    return {utf8To16Lossy(std::string_view(text))};
}

std::string text(const slint::SharedString& value)
{
    return std::string(std::string_view(value));
}

slint::SharedString shared(std::string_view value)
{
    return {value};
}

// The bar's dock item, made the bar's own: a dock that comes with the bar's style is first copied
// into the bar (that zone becomes its own). nullptr: no dock (or `make` is false and it is the style's).
domain::BarItem* dockOf(domain::BarConfig& bar, bool make)
{
    for (auto* zone : {&bar.left, &bar.center, &bar.right})
    {
        for (auto& item : *zone)
        {
            if (item.id == "dock")
            {
                return &item;
            }
        }
    }
    if (!make)
    {
        return nullptr;
    }
    const domain::BarConfig shown = domain::withStyle(bar);
    const std::array<std::pair<const std::vector<domain::BarItem>*, std::vector<domain::BarItem>*>, 3> zones = {
        {{&shown.left, &bar.left}, {&shown.center, &bar.center}, {&shown.right, &bar.right}}};
    constexpr std::array<std::string_view, 3> kKeys = {"left", "center", "right"};
    for (std::size_t i = 0; i < zones.size(); ++i)
    {
        if (std::ranges::any_of(*zones[i].first, [](const auto& item) { return item.id == "dock"; }))
        {
            *zones[i].second = *zones[i].first;
            settings::markKey(bar, kKeys[i]);
            return dockOf(bar, false);
        }
    }
    return nullptr;
}

std::vector<std::string> pinnedOf(const domain::BarItem& item)
{
    std::vector<std::string> ids;
    if (const auto* pinned = item.config.find("pinned"))
    {
        if (const auto* list = std::get_if<domain::Value::Array>(&pinned->data))
        {
            for (const auto& entry : *list)
            {
                if (const auto* id = std::get_if<std::string>(&entry.data))
                {
                    ids.push_back(*id);
                }
            }
        }
    }
    return ids;
}

void setPinned(domain::BarItem& item, const std::vector<std::string>& ids)
{
    if (!std::holds_alternative<domain::Value::Table>(item.config.data))
    {
        item.config.data = domain::Value::Table{};
    }
    auto& table = std::get<domain::Value::Table>(item.config.data);
    std::erase_if(table, [](const domain::ValueMember& member) { return member.key == "pinned"; });
    if (ids.empty())
    {
        return;
    }
    domain::Value::Array list;
    for (const auto& id : ids)
    {
        list.push_back(domain::Value{id});
    }
    table.push_back({"pinned", domain::Value{std::move(list)}});
}

template <class T>
std::shared_ptr<slint::VectorModel<T>> modelOf(std::vector<T> rows)
{
    return std::make_shared<slint::VectorModel<T>>(std::move(rows));
}

bool sameFolder(const fs::path& a, const fs::path& b)
{
    if (a.empty() || b.empty())
    {
        return false;
    }
    std::error_code ec;
    const auto left = fs::absolute(a, ec).lexically_normal().native();
    const auto right = fs::absolute(b, ec).lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::string localTime(std::int64_t unixSeconds)
{
    const auto seconds = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    _tzset(); // the time zone as it is now (the C runtime reads it once)
    if (localtime_s(&local, &seconds) != 0)
    {
        return {};
    }
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

std::string megabytes(std::uint64_t bytes)
{
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

int triState(const std::optional<bool>& value)
{
    return !value ? 0 : *value ? 1 : 2;
}

std::optional<bool> fromTriState(int value)
{
    return value == 1 ? std::optional(true) : value == 2 ? std::optional(false) : std::nullopt;
}

std::string timeText(domain::TimeOfDay time)
{
    return std::format("{:02}:{:02}", time.hour, time.minute);
}

// The image a wallpaper rule shows best: the image, the video's fallback (or the video itself),
// the first image of a slideshow.
fs::path previewOf(const fs::path& rice, const domain::MonitorWallpaper& rule)
{
    using Kind = domain::WallpaperKind;
    if (rule.kind == Kind::Color)
    {
        return {};
    }
    if (rule.kind == Kind::Video && !rule.fallbackImage.empty())
    {
        return settings::assetPath(rice, rule.fallbackImage);
    }
    fs::path source = settings::assetPath(rice, rule.source);
    if (rule.kind != Kind::Slideshow)
    {
        return source;
    }
    std::error_code ec;
    for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            return it->path();
        }
    }
    return {};
}

// Cursor files named like their role, for a folder without install.inf.
std::vector<domain::CursorAssignment> guessCursors(const fs::path& folder)
{
    struct Hint
    {
        domain::CursorRole role = domain::CursorRole::Normal;
        std::array<std::string_view, 3> words;
    };
    using R = domain::CursorRole;
    constexpr std::array<Hint, domain::kCursorRoleCount> kHints = {{
        {R::Normal, {"normal", "arrow", "pointer"}},
        {R::Help, {"help", "question", "help"}},
        {R::Working, {"working", "appstarting", "background"}},
        {R::Busy, {"busy", "wait", "loading"}},
        {R::Precision, {"precision", "cross", "crosshair"}},
        {R::Text, {"text", "beam", "ibeam"}},
        {R::Handwriting, {"handwriting", "pen", "nwpen"}},
        {R::Unavailable, {"unavailable", "no", "forbidden"}},
        {R::Vertical, {"vertical", "sizens", "ns"}},
        {R::Horizontal, {"horizontal", "sizewe", "ew"}},
        {R::Diagonal1, {"diagonal1", "sizenwse", "nwse"}},
        {R::Diagonal2, {"diagonal2", "sizenesw", "nesw"}},
        {R::Move, {"move", "sizeall", "all"}},
        {R::Alternate, {"alternate", "uparrow", "up"}},
        {R::Link, {"link", "hand", "link"}},
        {R::Location, {"location", "pin", "location"}},
        {R::Person, {"person", "user", "person"}},
    }};
    std::vector<domain::CursorAssignment> map;
    std::error_code ec;
    for (const auto& hint : kHints)
    {
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string stem = utf8(it->path().stem());
            std::ranges::transform(stem, stem.begin(), [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
            const std::string extension = utf8(it->path().extension());
            if ((extension == ".cur" || extension == ".ani" || extension == ".CUR" || extension == ".ANI") &&
                std::ranges::find(hint.words, stem) != hint.words.end())
            {
                map.push_back({hint.role, utf8(it->path().filename())});
                break;
            }
        }
    }
    return map;
}

} // namespace

// ---- the window ---------------------------------------------------------------------------------

namespace
{

// The one settings window (one instance per user): its original window procedure and its app.
WNDPROC g_original = nullptr;
SettingsApp* g_app = nullptr;
HWND g_window = nullptr;
bool g_sizing = false;

LRESULT CALLBACK repaintingProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    bool whole = false;
    switch (message)
    {
    case WM_ENTERSIZEMOVE:
        g_sizing = true;
        break;
    case WM_EXITSIZEMOVE:
        g_sizing = false;
        whole = true;
        break;
    case WM_WINDOWPOSCHANGED: {
        // Moved by code or a shortcut (Win+arrows), or shown again; a drag ends with EXITSIZEMOVE.
        const auto* position =
            reinterpret_cast<const WINDOWPOS*>(lParam); // NOLINT(performance-no-int-to-ptr)
        whole = !g_sizing && position != nullptr &&
                ((position->flags & SWP_NOMOVE) == 0 || (position->flags & SWP_SHOWWINDOW) != 0);
        break;
    }
    case WM_SIZE:
        whole = wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED;
        if (whole && g_app != nullptr)
        {
            const UINT dpi = GetDpiForWindow(window);
            g_app->setWidth(static_cast<double>(LOWORD(lParam)) * 96.0 / (dpi == 0 ? 96.0 : dpi));
        }
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        whole = true;
        break;
    case WM_SHOWWINDOW:
        whole = wParam != FALSE; // shown again (the tiler's workspaces hide and show it)
        break;
    case WM_ACTIVATE:
        whole = LOWORD(wParam) != WA_INACTIVE; // back from another desktop, Alt+Tab
        break;
    case WM_PAINT: {
        // Windows asks for an area (uncovered, shown again): the renderer would draw only what it knows
        // changed. Slint's own redraws come with no area, so this never feeds itself.
        RECT update{};
        whole = GetUpdateRect(window, &update, FALSE) != FALSE;
        break;
    }
    default:
        break;
    }
    const LRESULT result = CallWindowProcW(g_original, window, message, wParam, lParam);
    if (whole && g_app != nullptr)
    {
        g_app->repaintAll();
    }
    return result;
}

// Back from another virtual desktop: DWM uncloaks the window, and no window message says so.
void CALLBACK uncloaked(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
{
    if (object == OBJID_WINDOW && g_app != nullptr && window == g_window)
    {
        g_app->repaintAll();
    }
}

} // namespace

void SettingsApp::watchWindow()
{
    auto* const handle = static_cast<HWND>(window());
    if (handle == nullptr || g_original != nullptr)
    {
        return;
    }
    g_app = this;
    g_window = handle;
    g_original = reinterpret_cast<WNDPROC>( // NOLINT(performance-no-int-to-ptr): the window's procedure
        SetWindowLongPtrW(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&repaintingProc)));
    // This process's windows only (out of context: nothing is loaded anywhere), heard on this thread's loop.
    if (SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, &uncloaked, GetCurrentProcessId(),
                        0, WINEVENT_OUTOFCONTEXT) == nullptr)
    {
        log::warn("settings: cannot hear the window come back from another desktop ({})", GetLastError());
    }
}

void SettingsApp::setWidth(double logical)
{
    const bool narrow = logical < 1000.0;
    // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
    slint::invoke_from_event_loop([this, narrow] {
        if (m_ui->get_narrow() != narrow)
        {
            m_ui->set_narrow(narrow);
        }
    });
}

void SettingsApp::repaintAll()
{
    if (m_repaintQueued)
    {
        return;
    }
    m_repaintQueued = true;
    // Not from inside the window procedure: Slint takes it from its own loop.
    // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
    slint::invoke_from_event_loop([this] {
        m_repaintQueued = false;
        m_ui->set_repaint(!m_ui->get_repaint());
    });
}

SettingsApp::SettingsApp(fs::path dataDirectory)
    : m_dataDirectory(std::move(dataDirectory))
    , m_ui(AppWindow::create())
    , m_worker([this](const std::stop_token& stop) { work(stop); })
{
    const auto& api = m_ui->global<Api>();
    api.set_version(shared(kVersion));
    api.set_data_folder(shared(utf8(m_dataDirectory)));
    bind();
}

SettingsApp::~SettingsApp()
{
    m_worker.request_stop();
    m_wake.notify_all();
}

bool SettingsApp::stop(std::chrono::milliseconds patience)
{
    m_worker.request_stop();
    m_wake.notify_all();
    std::unique_lock lock(m_mutex);
    return m_stopped.wait_for(lock, patience, [this] { return m_workerDone; });
}

void* SettingsApp::window() const
{
    return m_ui->window().win32_hwnd();
}

void SettingsApp::run()
{
    applyLanguage();
    m_ui->window().on_close_requested([this] {
        log::info("settings: closing");
        flushEdits();
        return slint::CloseRequestResponse::HideWindow;
    });
    m_ui->show();
    // The native window comes with the event loop (winit creates it there): what needs it waits for it.
    // (v1.6 asked right here, got none, and the window never heard it came back from another workspace.)
    m_windowSetup.start(slint::TimerMode::Repeated, std::chrono::milliseconds(20), [this] {
        if (window() == nullptr)
        {
            return;
        }
        m_windowSetup.stop();
        fitToMonitor();
        watchWindow();
        setDarkTitleBar(window(), windowsUsesDarkApps());
        log::info("settings: the window is up");
    });
    log::info("settings: shown");
    notice(NoticeKind::Connecting);
    refreshStatus();
    refreshSettings();
    m_refresh.start(slint::TimerMode::Repeated, kRefreshEvery, [this] {
        const Page page = m_ui->global<Api>().get_page();
        if (page == Page::Home && !m_statusPending)
        {
            refreshStatus();
        }
        else if (page == Page::Diagnostics)
        {
            refreshDiagnostics();
        }
    });
    slint::run_event_loop();
    m_refresh.stop();
    flushEdits();
}

void SettingsApp::fitToMonitor()
{
    // At 200% a 1180x720 window is 2360x1440 pixels: on a smaller screen it would not fit. The
    // whole window (frame and title too) goes inside the monitor's work area, in its middle.
    auto* const handle = static_cast<HWND>(window());
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    RECT frame{};
    if (handle == nullptr || GetWindowRect(handle, &frame) == FALSE ||
        GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) == FALSE)
    {
        return;
    }
    const RECT& work = monitor.rcWork;
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int width = std::min<int>(frame.right - frame.left, workWidth * 96 / 100);
    const int height = std::min<int>(frame.bottom - frame.top, workHeight * 96 / 100);
    SetWindowPos(handle, nullptr, work.left + ((workWidth - width) / 2),
                 work.top + ((workHeight - height) / 2), width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void SettingsApp::hideForExit()
{
    log::info("settings: Ricing Manager is exiting; closing");
    flushEdits();
    m_ui->hide();
}

void SettingsApp::bringToFront()
{
    auto* const handle = static_cast<HWND>(window());
    if (handle == nullptr)
    {
        return;
    }
    if (IsIconic(handle) != FALSE)
    {
        ShowWindow(handle, SW_RESTORE);
    }
    SetForegroundWindow(handle);
}

void SettingsApp::notice(NoticeKind kind, const std::string& detail, bool canUndo)
{
    m_ui->global<Api>().set_notice(Notice{kind, shared(detail), canUndo});
}

void SettingsApp::failed(const Error& error)
{
    log::warn("settings: {}", error.toString());
    notice(NoticeKind::Failed, error.message());
}

void SettingsApp::refused(std::string_view why)
{
    notice(NoticeKind::Failed, text(m_ui->global<Words>().invoke_problem(shared(why))));
}

void SettingsApp::applyLanguage()
{
    const int choice = m_ui->global<Api>().get_language();
    m_appliedLanguage = choice;
    const bool portuguese = choice == 2 || (choice == 0 && windowsSpeaksPortuguese());
    slint::select_bundled_translation(portuguese ? "pt_BR" : "");
    // What C++ wrote with the old language.
    matchChoices();
    widgetChoices();
    if (m_edit)
    {
        showEdited();
    }
    if (!m_plugins.empty())
    {
        refreshPlugins();
    }
    const auto& api = m_ui->global<Api>();
    if (api.get_page() == Page::Diagnostics)
    {
        refreshDiagnostics(); // the processes' roles
    }
    if (api.get_engine_open())
    {
        const auto& words = m_ui->global<Words>();
        api.set_engine_status(m_engineScanning ? words.invoke_engine_looking()
                                               : words.invoke_engine_found(static_cast<int>(m_engine.size())));
        const auto items = std::static_pointer_cast<slint::VectorModel<EngineItem>>(api.get_engine_items());
        for (std::size_t i = 0; items && i < items->row_count() && i < m_engine.size(); ++i)
        {
            auto item = *items->row_data(i);
            const auto kind = shared(std::string(settings::toString(m_engine[i].kind)));
            item.kind = words.invoke_engine_kind(kind);
            item.note = words.invoke_engine_note(kind, m_engine[i].usable(), !m_engine[i].video.empty());
            items->set_row_data(i, item);
        }
    }
}

// ---- the core -----------------------------------------------------------------------------------

void SettingsApp::work(const std::stop_token& stop)
{
    // The connection to the core goes with this thread: closed when it ends.
    client::CoreClient client(m_dataDirectory);
    client.onReplace([this](bool replacing) { m_replacingCore = replacing; });
    const auto done = ScopeExit([this]() noexcept {
        const std::scoped_lock lock(m_mutex);
        m_workerDone = true;
        m_stopped.notify_all();
    });
    while (!stop.stop_requested())
    {
        Task task;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [this] { return !m_tasks.empty(); }))
            {
                return;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task(client, stop);
    }
}

bool SettingsApp::pause(const std::stop_token& stop, std::chrono::milliseconds time)
{
    std::unique_lock lock(m_mutex);
    m_wake.wait_for(lock, stop, time, [] { return false; });
    return stop.stop_requested();
}

void SettingsApp::call(std::string method, Json params, Done done, bool operation)
{
    if (operation)
    {
        ++m_operations;
        m_ui->global<Api>().set_busy(true);
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_tasks.emplace_back([this, method = std::move(method), params = std::move(params), done = std::move(done),
                              operation](client::CoreClient& client, const std::stop_token& stop) mutable {
            Reply reply = [&]() -> Reply {
                if (!client.connected())
                {
                    if (m_closing)
                    {
                        return fail(ErrorCode::NotFound, "Ricing Manager is exiting");
                    }
                    if (auto connected = client.connect(); !connected)
                    {
                        return std::unexpected(std::move(connected.error()));
                    }
                }
                const auto deadline = std::chrono::steady_clock::now() + 5min;
                while (true)
                {
                    // A package from anyone is unpacked by the core's import worker: up to 10 min.
                    const bool unpacks = method == "rice.import" || method == "plugin.install";
                    auto result = client.call(method, params,
                                              unpacks     ? std::chrono::milliseconds(11min)
                                              : operation ? std::chrono::milliseconds(5min)
                                                          : std::chrono::milliseconds(30s));
                    if (result || result.error().code() != ErrorCode::Busy || !operation ||
                        std::chrono::steady_clock::now() > deadline)
                    {
                        return result;
                    }
                    // The core is busy with another change: asked again in a moment, unless the
                    // window closed (v1.7 kept an invisible process waiting up to 5 minutes).
                    if (pause(stop, 1s))
                    {
                        return fail(ErrorCode::Cancelled, "the window closed");
                    }
                }
            }();
            slint::invoke_from_event_loop(
                [this, reply = std::move(reply), done = std::move(done), operation]() mutable {
                    if (operation)
                    {
                        --m_operations;
                        m_ui->global<Api>().set_busy(m_operations > 0);
                    }
                    done(std::move(reply));
                });
        });
    }
    m_wake.notify_one();
}

void SettingsApp::background(std::function<Reply()> job, Done done, bool busy)
{
    if (busy)
    {
        ++m_operations;
        m_ui->global<Api>().set_busy(true);
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_tasks.emplace_back([this, job = std::move(job), done = std::move(done),
                              busy](client::CoreClient&, const std::stop_token&) mutable {
            Reply reply = job();
            slint::invoke_from_event_loop([this, reply = std::move(reply), done = std::move(done), busy]() mutable {
                if (busy)
                {
                    --m_operations;
                    m_ui->global<Api>().set_busy(m_operations > 0);
                }
                done(std::move(reply));
            });
        });
    }
    m_wake.notify_one();
}

// ---- what the pages show ---------------------------------------------------------------------------

void SettingsApp::refreshStatus()
{
    if (m_statusPending)
    {
        m_statusAgain = true;
        return;
    }
    m_statusPending = true;
    call("status.get", Json::object(), [this](Reply reply) {
        m_statusPending = false;
        if (std::exchange(m_statusAgain, false))
        {
            refreshStatus(); // asked for while this one was on its way: something changed since
        }
        const auto& api = m_ui->global<Api>();
        if (!reply)
        {
            log::warn("settings: status: {}", reply.error().toString());
            api.set_connected(false);
            notice(NoticeKind::NotConnected, reply.error().message());
            return;
        }
        const bool first = !api.get_connected();
        if (first)
        {
            log::info("settings: connected to the core");
        }
        api.set_connected(true);
        if (api.get_notice().kind == NoticeKind::Connecting ||
            api.get_notice().kind == NoticeKind::NotConnected)
        {
            notice(NoticeKind::None);
        }
        m_status = std::move(*reply);

        const Json rice = json::memberOr(m_status, "rice", Json::object());
        const std::string current = json::stringOr(rice, "current", "");
        // Its name comes from the library (read on the worker thread): not a file read every 3 s.
        if (current != text(api.get_active_folder()))
        {
            api.set_active_folder(shared(current));
            const fs::path folder(utf8To16Lossy(current));
            const auto entry = std::ranges::find_if(
                m_riceEntries, [&](const settings::RiceEntry& e) { return sameFolder(e.folder, folder); });
            api.set_active_name(current.empty()            ? slint::SharedString()
                                : entry != m_riceEntries.end() ? shared(entry->name)
                                                               : shared(utf8(folder.filename())));
            if (!first)
            {
                refreshRices(); // which one is applied, and its name as its rice.toml says
            }
        }
        api.set_can_undo(!json::stringOr(rice, "previous", "").empty());
        api.set_safe_mode(json::memberOr(m_status, "safeMode", Json()).is_string());
        if (const Page page = api.get_page(); page == Page::Appearance || page == Page::Wallpapers ||
                                              page == Page::Cursors || page == Page::Sounds || page == Page::Icons ||
                                              page == Page::Bar)
        {
            ensureEditing(page); // opened on an editor page before the core answered
        }
        api.set_original_captured(json::memberOr(m_status, "originalCapturedAt", Json()).is_number());

        std::vector<slint::SharedString> managed;
        std::string managedNames;
        for (const auto& module : json::memberOr(m_status, "managedModules", Json::array()))
        {
            if (module.is_string())
            {
                managed.push_back(shared(module.get<std::string>()));
                managedNames += (managedNames.empty() ? "" : " · ") +
                                text(m_ui->global<Words>().invoke_module(managed.back()));
            }
        }
        api.set_managed(modelOf(std::move(managed)));
        api.set_managed_names(shared(managedNames));

        std::vector<HistoryRow> history;
        for (const auto& entry : json::memberOr(m_status, "history", Json::array()))
        {
            history.push_back({static_cast<int>(json::intOr(entry, "id", 0)),
                               shared(json::stringOr(entry, "label", "")),
                               shared(localTime(json::intOr(entry, "appliedAt", 0)))});
        }
        api.set_history(modelOf(std::move(history)));

        std::vector<StatusLine> lines;
        const Json bar = json::memberOr(m_status, "bar", Json::object());
        if (json::boolOr(bar, "configured", false))
        {
            const char* state = json::boolOr(bar, "off", false)        ? "off"
                                : json::boolOr(bar, "disabled", false) ? "disabled"
                                : json::boolOr(bar, "running", false)  ? "running"
                                                                       : "starting";
            lines.push_back({shared("bar"), shared(state), {}});
        }
        const Json video = json::memberOr(m_status, "video", Json::object());
        if (json::boolOr(video, "configured", false))
        {
            std::string detail = json::stringOr(video, "error", json::stringOr(video, "adapter", ""));
            for (const auto& monitor : json::memberOr(video, "monitors", Json::array()))
            {
                const std::string error = json::stringOr(monitor, "error", "");
                if (!error.empty())
                {
                    detail += (detail.empty() ? "" : " · ") + error;
                }
            }
            const char* state = json::boolOr(video, "disabled", false)  ? "disabled"
                                : json::boolOr(video, "running", false) ? "running"
                                                                        : "starting";
            lines.push_back({shared("video"), shared(state), shared(detail)});
        }
        if (const Json schedule = json::memberOr(m_status, "schedule", Json()); schedule.is_object())
        {
            lines.push_back({shared("schedule"), shared("on"),
                             shared(std::format("{} · {}", json::stringOr(schedule, "lightAt", "?"),
                                                json::stringOr(schedule, "darkAt", "?")))});
        }
        std::string tools;
        for (const auto& tool : json::memberOr(m_status, "otherTools", Json::array()))
        {
            const std::string name =
                tool.is_string() ? tool.get<std::string>() : json::stringOr(tool, "name", "");
            if (!name.empty())
            {
                tools += (tools.empty() ? "" : ", ") + name;
            }
        }
        if (!tools.empty())
        {
            lines.push_back({shared("tools"), shared("running"), shared(tools)});
        }
        api.set_status_lines(modelOf(std::move(lines)));

        if (m_edit)
        {
            api.set_edit_active(sameFolder(m_edit->folder, fs::path(utf8To16Lossy(current))));
        }
        // What the core says of the hotkeys now (taken by another program...): shown with this
        // status, not with the next save.
        showHotkeys();
        if (first)
        {
            refreshRices();
            refreshPlugins();
            refreshMonitors();
            call("action.list", Json::object(), [this](Reply actions) {
                if (actions && actions->is_array())
                {
                    m_actionNames.clear();
                    for (const auto& name : *actions)
                    {
                        if (name.is_string())
                        {
                            m_actionNames.push_back(name.get<std::string>());
                        }
                    }
                    refreshSettings();
                }
            });
            call("autostart.get", Json::object(), [this](Reply state) {
                if (state)
                {
                    m_ui->global<Api>().set_autostart(json::boolOr(*state, "enabled", false));
                }
            });
        }
    });
}

void SettingsApp::refreshRices()
{
    if (m_ricesPending)
    {
        m_ricesAgain = true;
        return;
    }
    m_ricesPending = true;
    const std::string asked = json::stringOr(json::memberOr(m_status, "rice", Json::object()), "current", "");
    auto found = std::make_shared<std::vector<settings::RiceEntry>>();
    auto remembered = std::make_shared<std::vector<fs::path>>();
    background(
        // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
        [this, asked, found, remembered]() -> Reply {
            *remembered = rememberedFolders();
            auto extra = *remembered;
            if (!asked.empty())
            {
                extra.emplace_back(utf8To16Lossy(asked));
            }
            *found = settings::scanLibrary(m_dataDirectory, extra);
            return Json::object();
        },
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        [this, found, remembered](const Reply&) {
            m_ricesPending = false;
            if (std::exchange(m_ricesAgain, false))
            {
                refreshRices();
            }
            m_riceEntries = std::move(*found);
            showRices(*remembered);
        },
        false);
}

void SettingsApp::showRices(const std::vector<fs::path>& remembered)
{
    const auto& entries = m_riceEntries;
    const auto& api = m_ui->global<Api>();
    const fs::path current(
        utf8To16Lossy(json::stringOr(json::memberOr(m_status, "rice", Json::object()), "current", "")));
    const fs::path library = settings::ricesFolder(m_dataDirectory);
    std::vector<RiceRow> rows;
    rows.reserve(entries.size());
    for (const auto& entry : entries)
    {
        RiceRow row{};
        row.name = shared(entry.name);
        row.folder = shared(utf8(entry.folder));
        row.author = shared(entry.author);
        row.description = shared(entry.description);
        row.problem = shared(entry.problem);
        row.active = sameFolder(entry.folder, current);
        row.shareable = entry.shareable;
        // A folder the user added (not the library's): it can be forgotten.
        row.remembered = !sameFolder(entry.folder.parent_path(), library) &&
                         std::ranges::any_of(remembered, [&](const fs::path& f) { return sameFolder(f, entry.folder); });
        if (row.active)
        {
            api.set_active_name(row.name);
        }
        rows.push_back(std::move(row));
    }
    auto model = modelOf(std::move(rows));
    api.set_rices(model);
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const std::weak_ptr<slint::VectorModel<RiceRow>> weak = model;
        m_thumbnails.request(entries[i].preview, kPreviewWidth, kPreviewHeight,
                             [weak, i](const slint::Image& image) {
                                 if (auto rows = weak.lock(); rows && i < rows->row_count())
                                 {
                                     auto row = *rows->row_data(i);
                                     row.preview = image;
                                     row.has_preview = true;
                                     rows->set_row_data(i, row);
                                 }
                             });
    }
}

void SettingsApp::refreshPlugins()
{
    call("plugin.list", Json::object(), [this](Reply reply) {
        if (!reply || !reply->is_array())
        {
            return;
        }
        m_plugins = std::move(*reply);
        const auto& api = m_ui->global<Api>();
        const auto& words = m_ui->global<Words>();
        const domain::Rice* rice = edited();
        std::vector<PluginRow> rows;
        for (const auto& entry : m_plugins)
        {
            PluginRow row{};
            const std::string id = json::stringOr(entry, "id", "");
            row.id = shared(id);
            row.name = shared(json::stringOr(entry, "name", id));
            row.version = shared(json::stringOr(entry, "version", ""));
            row.description = shared(json::stringOr(entry, "description", ""));
            row.dev = json::boolOr(entry, "dev", false);
            const Json host = json::memberOr(entry, "host", Json());
            std::string state = "off";
            std::string detail;
            if (const auto problem = json::stringOr(entry, "problem", ""); !problem.empty())
            {
                state = "broken";
                detail = problem;
            }
            else if (const auto disabled = json::stringOr(entry, "disabled", ""); !disabled.empty())
            {
                state = "stopped";
                detail = disabled;
            }
            else if (json::boolOr(entry, "needsApproval", false))
            {
                state = "needs-approval";
            }
            else if (host.is_object() && !json::stringOr(host, "error", "").empty())
            {
                state = "stopped";
                detail = json::stringOr(host, "error", "");
            }
            else if (host.is_object())
            {
                state = "running";
            }
            else if (json::boolOr(entry, "enabled", false))
            {
                state = "enabled";
            }
            row.state = shared(state);
            row.detail = shared(detail);
            for (const auto& kind : json::memberOr(entry, "kinds", Json::array()))
            {
                if (kind == "action" || kind == "rule")
                {
                    row.can_enable = true;
                }
            }
            std::vector<Permission> permissions;
            auto asked = plugins::permissionsFromJson(json::memberOr(entry, "permissions", Json::object()));
            if (!asked)
            {
                // Nothing to show is nothing to approve: an approval would grant what nobody saw.
                state = "broken";
                row.state = shared(state);
                row.detail = m_ui->global<Words>().invoke_problem(shared("permissions-unreadable"));
                log::warn("settings: the permissions of {} do not read: {}", id, asked.error().toString());
            }
            else
            {
                for (const auto& provider : asked->providers)
                {
                    const auto* info = domain::findPluginProvider(provider);
                    permissions.push_back({shared(provider), words.invoke_permission(shared(provider)),
                                           info != nullptr && info->sensitive});
                }
                for (const auto& action : asked->actions)
                {
                    const auto* info = domain::findPluginAction(action);
                    permissions.push_back({shared(action), words.invoke_permission(shared(action)),
                                           info != nullptr && info->sensitive});
                }
                for (const auto& domain : asked->network)
                {
                    permissions.push_back(
                        {shared("network:" + domain), words.invoke_network(shared(domain)), true});
                }
                if (asked->storage)
                {
                    permissions.push_back(
                        {shared("storage"), words.invoke_permission(shared("storage")), false});
                }
            }
            row.permissions = modelOf(std::move(permissions));
            const fs::path folder(utf8To16Lossy(json::stringOr(entry, "folder", "")));
            if (auto manifest = plugins::readPluginManifest(folder))
            {
                std::string authors;
                for (const auto& author : manifest->manifest.authors)
                {
                    authors += (authors.empty() ? "" : ", ") + author;
                }
                row.author = shared(authors);
                row.has_config = !manifest->manifest.config.fields.empty();
            }
            row.in_rice = rice != nullptr && std::ranges::any_of(rice->pluginRequirements,
                                                                 [&](const auto& r) { return r.id == id; });
            rows.push_back(std::move(row));
        }
        // The selected plugin is the same one wherever it sits now (one before it may be gone).
        const auto selected = std::ranges::find_if(
            rows, [this](const PluginRow& row) { return std::string_view(row.id) == m_selectedPlugin; });
        if (selected == rows.end())
        {
            m_selectedPlugin.clear();
        }
        api.set_selected_plugin(selected == rows.end() ? -1 : static_cast<int>(selected - rows.begin()));
        api.set_plugins(modelOf(std::move(rows)));
        widgetChoices();
        refreshSettings(); // plugins' actions
        pluginConfig();
    });
}

void SettingsApp::refreshMonitors()
{
    call("monitors.list", Json::object(), [this](Reply reply) {
        if (reply && reply->is_array())
        {
            m_monitors = std::move(*reply);
            matchChoices();
            if (m_edit)
            {
                showWallpapers();
                showBars();
            }
        }
    });
}

void SettingsApp::matchChoices()
{
    const auto& words = m_ui->global<Words>();
    m_matchChoices.clear();
    std::vector<slint::SharedString> labels;
    for (const auto selector : kSelectors)
    {
        m_matchChoices.emplace_back(selector);
        labels.push_back(words.invoke_match(shared(selector)));
    }
    const auto add = [&](const std::string& key, const std::string& label) {
        const std::string choice = "edid:" + key;
        if (std::ranges::find(m_matchChoices, choice) == m_matchChoices.end())
        {
            m_matchChoices.push_back(choice);
            labels.push_back(shared(label));
        }
    };
    for (const auto& monitor : m_monitors)
    {
        add(json::stringOr(monitor, "key", ""),
            std::format("{} ({}×{})", json::stringOr(monitor, "name", "?"), json::intOr(monitor, "width", 0),
                        json::intOr(monitor, "height", 0)));
    }
    // A monitor the rice names that is not connected now.
    if (const domain::Rice* rice = edited())
    {
        const auto keep = [&](const domain::MonitorSelector& selector) {
            if (selector.kind == domain::MonitorSelector::Kind::Edid)
            {
                add(selector.edidKey, selector.edidKey);
            }
        };
        if (rice->wallpaper)
        {
            for (const auto& rule : rice->wallpaper->monitors)
            {
                keep(rule.match);
            }
        }
        for (const auto& bar : rice->bars)
        {
            keep(bar.monitor);
        }
    }
    std::vector<slint::SharedString> choices;
    choices.reserve(m_matchChoices.size());
    for (const auto& choice : m_matchChoices)
    {
        choices.push_back(shared(choice));
    }
    const auto& api = m_ui->global<Api>();
    api.set_match_choices(modelOf(std::move(choices)));
    api.set_match_labels(modelOf(std::move(labels)));
}

void SettingsApp::widgetChoices()
{
    const auto& words = m_ui->global<Words>();
    std::vector<WidgetChoice> choices;
    std::vector<slint::SharedString> labels;
    for (const auto id : settings::builtinWidgets())
    {
        choices.push_back({shared(id), words.invoke_widget(shared(id))});
        labels.push_back(choices.back().label);
    }
    for (const auto& entry : m_plugins)
    {
        const auto kinds = json::memberOr(entry, "kinds", Json::array());
        if (std::ranges::find(kinds, Json("bar.widget")) != kinds.end())
        {
            const std::string id = json::stringOr(entry, "id", "");
            choices.push_back({shared(id), shared(json::stringOr(entry, "name", id))});
            labels.push_back(choices.back().label);
        }
    }
    const auto& api = m_ui->global<Api>();
    api.set_widget_choices(modelOf(std::move(choices)));
    api.set_widget_labels(modelOf(std::move(labels)));
}

std::string SettingsApp::pluginName(std::string_view id) const
{
    for (const auto& entry : m_plugins)
    {
        if (json::stringOr(entry, "id", "") == id)
        {
            return json::stringOr(entry, "name", std::string(id));
        }
    }
    return std::string(id);
}

std::vector<std::string> SettingsApp::knownActions() const
{
    if (!m_actionNames.empty())
    {
        return m_actionNames;
    }
    return {kBuiltinActions.begin(), kBuiltinActions.end()};
}

void SettingsApp::refreshSettings()
{
    const auto& api = m_ui->global<Api>();
    const auto& words = m_ui->global<Words>();
    const auto names = knownActions();
    const std::vector<std::string_view> views(names.begin(), names.end());
    auto loaded = settings::loadSettings(m_dataDirectory, views);
    if (!loaded)
    {
        log::warn("settings: settings.toml: {}", loaded.error().toString());
        m_settings.reset();
        api.set_settings_problem(shared(loaded.error().message()));
    }
    else
    {
        m_settings = std::move(*loaded);
        api.set_settings_problem({});
    }

    // Actions: ours, then the plugins'.
    std::vector<ActionChoice> actions;
    std::vector<slint::SharedString> labels;
    for (const auto& name : names)
    {
        actions.push_back({shared(name), words.invoke_action(shared(name))});
        labels.push_back(actions.back().label);
    }
    for (const auto& entry : m_plugins)
    {
        const std::string plugin = json::stringOr(entry, "name", json::stringOr(entry, "id", ""));
        for (const auto& action : json::memberOr(entry, "actions", Json::array()))
        {
            actions.push_back({shared(json::stringOr(action, "name", "")),
                               shared(std::format("{}: {}", plugin, json::stringOr(action, "label", "")))});
            labels.push_back(actions.back().label);
        }
    }
    api.set_actions(modelOf(std::move(actions)));
    api.set_action_labels(modelOf(std::move(labels)));

    if (!m_settings)
    {
        return;
    }
    std::vector<RuleRow> rules;
    for (const auto& rule : m_settings->rules)
    {
        rules.push_back({static_cast<int>(rule.trigger), shared(rule.at ? timeText(*rule.at) : std::string()),
                         shared(rule.monitor ? rule.monitor->toString() : std::string()),
                         shared(rule.action.name), shared(rule.action.ricePath), shared(rule.action.app),
                         shared(appName(rule.action.app))});
    }
    api.set_rules_list(modelOf(std::move(rules)));
    api.set_keep_in_tray(m_settings->keepInTray);
    api.set_start_win_key(m_settings->startWinKey);
    api.set_machine_icons(m_settings->machineIcons);
    const domain::TilingSettings& tiling = m_settings->tiling;
    api.set_tiling_enabled(tiling.enabled);
    api.set_tiling_mode(tiling.mode == domain::TilingMode::Mac ? 1 : 0);
    api.set_tiling_layout(static_cast<int>(tiling.layout));
    api.set_tiling_workspaces(tiling.workspaces);
    api.set_tiling_gap(tiling.gap);
    api.set_tiling_outer_gap(tiling.outerGap);
    api.set_tiling_master(tiling.masterPercent);
    api.set_tiling_border(tiling.border);
    api.set_tiling_admin(tiling.runAsAdmin);
    api.set_tiling_animations(tiling.animations);
    api.set_tiling_border_color(shared(tiling.borderColor ? tiling.borderColor->toHex() : std::string()));
    std::vector<TilingKeyRow> tilingKeys;
    tilingKeys.reserve(domain::kTilingCommandCount);
    for (std::size_t i = 0; i < domain::kTilingCommandCount; ++i)
    {
        const auto command = static_cast<domain::TilingCommand>(i);
        const auto key = std::ranges::find(tiling.keys, command, &domain::TilingKey::command);
        tilingKeys.push_back({shared(domain::toString(command)),
                              shared(key == tiling.keys.end() ? std::string() : key->toString()), shared("")});
    }
    api.set_tiling_keys(modelOf(std::move(tilingKeys)));
    std::vector<TilingRuleRow> tilingRules;
    tilingRules.reserve(tiling.rules.size());
    for (const auto& rule : tiling.rules)
    {
        tilingRules.push_back({shared(rule.app), shared(rule.windowClass), shared(rule.title),
                               static_cast<int>(rule.action), rule.workspace});
    }
    api.set_tiling_rules(modelOf(std::move(tilingRules)));
    api.set_language(m_settings->language == domain::kLanguageEnglish      ? 1
                     : m_settings->language == domain::kLanguagePortuguese ? 2
                                                                           : 0);
    showHotkeys(true);
    if (api.get_language() != m_appliedLanguage)
    {
        applyLanguage(); // settings.toml chose another
    }
}

void SettingsApp::showHotkeys(bool force)
{
    if (!m_settings)
    {
        return;
    }
    // What the core says of each hotkey (taken by another program...).
    std::map<std::string, std::string> problems;
    for (const auto& hotkey :
         json::memberOr(json::memberOr(m_status, "resident", Json::object()), "hotkeys", Json::array()))
    {
        if (!json::boolOr(hotkey, "registered", true))
        {
            problems[json::stringOr(hotkey, "keys", "")] = json::stringOr(hotkey, "error", "not active");
        }
    }
    // The same as shown: the rows stay (one could be being typed in).
    if (!force && problems == m_hotkeyProblems)
    {
        return;
    }
    m_hotkeyProblems = problems;
    std::vector<HotkeyRow> hotkeys;
    for (const auto& hotkey : m_settings->hotkeys)
    {
        const std::string keys = hotkey.keys.toString();
        hotkeys.push_back({shared(keys), shared(hotkey.action.name), shared(hotkey.action.ricePath),
                           shared(hotkey.action.app), shared(appName(hotkey.action.app)),
                           shared(problems.contains(keys) ? problems[keys] : std::string())});
    }
    m_ui->global<Api>().set_hotkeys(modelOf(std::move(hotkeys)));
}

void SettingsApp::refreshDiagnostics()
{
    if (m_diagnosticsPending)
    {
        return;
    }
    m_diagnosticsPending = true;
    // Every log is read: on the worker thread (m_processes is only sampled there).
    struct Read
    {
        std::vector<ProcessSample> samples;
        std::vector<std::string> problems;
    };
    auto read = std::make_shared<Read>();
    background(
        // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
        [this, read]() -> Reply {
            read->samples = m_processes.sample();
            read->problems = recentProblems(m_dataDirectory, kProblemsShown);
            return Json::object();
        },
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        [this, read](const Reply&) {
            m_diagnosticsPending = false;
            const auto& words = m_ui->global<Words>();
            std::vector<ProcessRow> rows;
            for (const auto& sample : read->samples)
            {
                const std::string role = sample.name == "rmw-core.exe"          ? "core"
                                         : sample.name == "rmw-bar.exe"         ? "bar"
                                         : sample.name == "rmw-wallpaper.exe"   ? "video"
                                         : sample.name == "rmw-plugin-host.exe" ? "plugins"
                                         : sample.name == "rmw-settings.exe"    ? "settings"
                                         : sample.name == "rmw-tiler.exe"       ? "tiler"
                                                                                : "";
                rows.push_back({shared(sample.name), static_cast<int>(sample.pid),
                                shared(megabytes(sample.privateBytes)), shared(std::format("{:.1f}%", sample.cpuPercent)),
                                role.empty() ? slint::SharedString() : words.invoke_worker(shared(role))});
            }
            const auto& api = m_ui->global<Api>();
            api.set_processes(modelOf(std::move(rows)));
            std::vector<slint::SharedString> problems;
            problems.reserve(read->problems.size());
            for (const auto& line : read->problems)
            {
                problems.push_back(shared(line));
            }
            api.set_recent_problems(modelOf(std::move(problems)));
        },
        false);
}

// ---- the rice being edited -----------------------------------------------------------------------

void SettingsApp::ensureEditing(Page page)
{
    const auto& api = m_ui->global<Api>();
    if (m_edit || m_preparingEdit || !api.get_connected())
    {
        return;
    }
    // The applied rice is what changes...
    const std::string current =
        json::stringOr(json::memberOr(m_status, "rice", Json::object()), "current", "");
    std::error_code ec;
    if (!current.empty() && fs::exists(fs::path(utf8To16Lossy(current)) / L"rice.toml", ec))
    {
        startEditing(fs::path(utf8To16Lossy(current)), page);
        return;
    }
    // ...or, with none applied, the look Windows has now, saved as "My look": these pages never
    // open on nothing to change. A "My look" made before is the one edited again (v1.7 made one
    // more each time the window opened with no rice applied).
    const std::string myLook = text(m_ui->global<Words>().invoke_my_look_name());
    const fs::path earlier =
        settings::ricesFolder(m_dataDirectory) / fs::path(utf8To16Lossy(domain::safeFileName(myLook, "Rice")));
    if (fs::exists(earlier / L"rice.toml", ec) && settings::loadRice(earlier))
    {
        startEditing(earlier, page);
        return;
    }
    auto folder = settings::newRiceFolder(m_dataDirectory, myLook);
    if (!folder)
    {
        failed(folder.error());
        return;
    }
    m_preparingEdit = true;
    api.set_preparing_edit(true);
    const std::string riceName = utf8(folder->filename());
    call(
        "rice.export", Json{{"path", utf8(*folder)}, {"name", riceName}},
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        [this, folder = *folder](Reply reply) {
            m_preparingEdit = false;
            m_ui->global<Api>().set_preparing_edit(false);
            if (!reply)
            {
                failed(reply.error());
                return;
            }
            refreshRices();
            if (m_edit)
            {
                return; // the user picked a rice to edit meanwhile: theirs wins
            }
            // Ready to edit; the window stays where the user went meanwhile (v1.7 took them back).
            const Page now = m_ui->global<Api>().get_page();
            const bool editorPage = now == Page::Appearance || now == Page::Wallpapers ||
                                    now == Page::Cursors || now == Page::Sounds || now == Page::Icons ||
                                    now == Page::Bar;
            startEditing(folder, editorPage ? std::optional(now) : std::nullopt);
        },
        true);
}

void SettingsApp::startEditing(const fs::path& folder, std::optional<Page> page)
{
    flushEdits(); // what the rice edited until now still waited to be saved
    auto loaded = settings::loadRice(folder);
    if (!loaded)
    {
        failed(loaded.error());
        return;
    }
    m_edit = Edit{folder, std::move(*loaded)};
    const auto& api = m_ui->global<Api>();
    api.set_selected_rule(-1);
    api.set_selected_bar(m_edit->loaded.rice.bars.empty() ? -1 : 0);
    api.set_renaming(false);
    m_selectedPlugin.clear();
    api.set_selected_plugin(-1);
    matchChoices();
    showEdited();
    refreshPlugins();
    if (page)
    {
        api.set_page(*page);
    }
}

bool SettingsApp::stillEditing(const fs::path& folder) const
{
    return m_edit && sameFolder(m_edit->folder, folder);
}

void SettingsApp::flushEdits()
{
    if (m_barSave.running())
    {
        m_barSave.stop();
        saveEdited();
    }
}

void SettingsApp::importInto(fs::path source, settings::AssetKind kind, std::function<void(const std::string&)> use)
{
    if (!m_edit)
    {
        return;
    }
    const fs::path folder = m_edit->folder;
    notice(NoticeKind::Copying, utf8(source.filename()));
    auto stored = std::make_shared<std::string>();
    background(
        // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
        [folder, source = std::move(source), kind, stored]() -> Reply {
            auto imported = settings::importAsset(folder, source, kind);
            if (!imported)
            {
                return std::unexpected(std::move(imported.error()));
            }
            *stored = std::move(*imported);
            return Json::object();
        },
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        [this, folder, stored, use = std::move(use)](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
                if (stillEditing(folder))
                {
                    showWallpapers(); // a kind picked for the file shows the rule's own again
                }
                return;
            }
            // Another rice is edited now: the copy stays in the first one's folder, unused.
            if (!stillEditing(folder))
            {
                notice(NoticeKind::None);
                return;
            }
            notice(NoticeKind::Imported, *stored);
            use(*stored);
        });
}

void SettingsApp::showEdited()
{
    const auto& api = m_ui->global<Api>();
    if (!m_edit)
    {
        api.set_editing(false);
        return;
    }
    const domain::Rice& rice = m_edit->loaded.rice;
    api.set_editing(true);
    api.set_edit_name(shared(rice.meta.name));
    api.set_edit_folder(shared(utf8(m_edit->folder)));
    api.set_edit_hand_written(m_edit->loaded.handWritten);
    api.set_edit_active(
        sameFolder(m_edit->folder, fs::path(utf8To16Lossy(json::stringOr(
                                       json::memberOr(m_status, "rice", Json::object()), "current", "")))));
    std::vector<slint::SharedString> warnings;
    for (const auto& item : m_edit->loaded.diagnostics.all())
    {
        if (warnings.size() < 5)
        {
            warnings.push_back(shared(item.toString("rice.toml")));
        }
    }
    api.set_edit_warnings(modelOf(std::move(warnings)));
    // What the fields said of the last thing typed in them: the rice's own values are shown now.
    api.set_schedule_problem({});
    api.set_accent_problem({});
    api.set_windows_border_problem({});
    api.set_windows_caption_problem({});
    api.set_windows_title_problem({});
    api.set_windows_exclude_problem({});
    api.set_rename_problem({});

    // Appearance.
    const auto& appearance = rice.appearance;
    int mode = 0;
    if (appearance && appearance->mode)
    {
        using M = domain::ThemeMode;
        mode = *appearance->mode == M::Light    ? 1
               : *appearance->mode == M::Dark   ? 2
               : *appearance->mode == M::System ? 3
                                                : 4;
    }
    api.set_theme_mode(mode);
    const domain::AppearanceSchedule schedule =
        appearance && appearance->schedule ? *appearance->schedule : domain::AppearanceSchedule{};
    api.set_light_at(shared(timeText(schedule.lightAt)));
    api.set_dark_at(shared(timeText(schedule.darkAt)));
    api.set_accent(appearance && appearance->accent ? shared(appearance->accent->toHex())
                                                    : slint::SharedString());
    api.set_transparency(appearance ? triState(appearance->transparency) : 0);
    api.set_accent_on_taskbar(appearance ? triState(appearance->accentOnTaskbar) : 0);
    std::vector<SwatchColor> palette;
    palette.reserve(rice.palette.size());
    for (const auto& entry : rice.palette)
    {
        palette.push_back(
            {slint::Color::from_argb_uint8(entry.color.a, entry.color.r, entry.color.g, entry.color.b),
             shared(entry.color.toHex())});
    }
    api.set_palette_colors(modelOf(std::move(palette)));

    // Cursors.
    api.set_cursors_managed(rice.cursors.has_value());
    api.set_cursors_source(rice.cursors ? shared(rice.cursors->source) : slint::SharedString());
    int size = 0;
    if (rice.cursors && rice.cursors->size)
    {
        const auto found = std::ranges::find(kCursorSizes, *rice.cursors->size);
        size = found == kCursorSizes.end() ? 0 : static_cast<int>(found - kCursorSizes.begin());
    }
    api.set_cursor_size(size);
    auto previews = modelOf(std::vector<CursorPreview>{});
    api.set_cursor_previews(previews);
    if (rice.cursors)
    {
        const fs::path folder = settings::assetPath(m_edit->folder, rice.cursors->source);
        std::error_code ec;
        std::size_t shown = 0;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end && shown < 8; it.increment(ec))
        {
            const auto extension = utf8(it->path().extension());
            if (extension != ".cur" && extension != ".ani" && extension != ".CUR" && extension != ".ANI")
            {
                continue;
            }
            ++shown;
            const std::weak_ptr<slint::VectorModel<CursorPreview>> weak = previews;
            const std::string role = utf8(it->path().stem());
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            m_thumbnails.request(it->path(), 48, 48, [weak, role](const slint::Image& image) {
                if (auto rows = weak.lock())
                {
                    rows->push_back({shared(role), image});
                }
            });
        }
    }

    // Sounds (M14).
    std::vector<SoundRow> sounds;
    sounds.reserve(domain::kSoundEventCount);
    for (std::size_t i = 0; i < domain::kSoundEventCount; ++i)
    {
        const auto event = static_cast<domain::SoundEvent>(i);
        const domain::SoundAssignment* set = nullptr;
        if (rice.sounds)
        {
            const auto found = std::ranges::find(rice.sounds->events, event, &domain::SoundAssignment::event);
            set = found == rice.sounds->events.end() ? nullptr : &*found;
        }
        sounds.push_back({shared(domain::riceName(event)), shared(set != nullptr ? set->file : std::string()),
                          set != nullptr && set->file.empty()});
    }
    api.set_sound_rows(modelOf(std::move(sounds)));
    api.set_sounds_silence(rice.sounds && rice.sounds->scheme == domain::SoundScheme::None);

    // Other apps' windows (M14).
    const auto& look = rice.windows;
    const auto refText = [](const std::optional<domain::ColorRef>& ref) {
        return ref ? shared(ref->toString()) : slint::SharedString();
    };
    api.set_windows_corners(look && look->corners ? static_cast<int>(*look->corners) + 1 : 0);
    api.set_windows_backdrop(look && look->backdrop ? static_cast<int>(*look->backdrop) + 1 : 0);
    api.set_windows_dark(look ? triState(look->dark) : 0);
    api.set_windows_border(look && look->noBorder ? slint::SharedString("none") : refText(look ? look->border : std::nullopt));
    api.set_windows_caption(refText(look ? look->caption : std::nullopt));
    api.set_windows_title(refText(look ? look->title : std::nullopt));
    std::string excluded;
    if (look)
    {
        for (const auto& program : look->exclude)
        {
            excluded += (excluded.empty() ? "" : ", ") + program;
        }
    }
    api.set_windows_exclude(shared(excluded));

    // The lock screen (M14).
    const auto& lock = rice.lockscreen;
    api.set_lock_image(shared(lock ? lock->image : std::string()));
    api.set_lock_spotlight(lock && lock->spotlight == true);
    api.set_lock_tips(lock && lock->tips ? (*lock->tips ? 1 : 2) : 0);

    // Icons (M14): every desktop icon and folder, the programs the rice names.
    const auto iconOf = [&rice](const auto& entries, const auto& key) {
        if (!rice.icons)
        {
            return std::string();
        }
        const auto& list = entries(*rice.icons);
        const auto found = std::ranges::find(list, key, [](const auto& entry) { return entry.first; });
        return found == list.end() ? std::string() : found->second;
    };
    std::vector<IconRow> desktopIcons;
    for (std::size_t i = 0; i < domain::kDesktopIconCount; ++i)
    {
        const auto icon = static_cast<domain::DesktopIcon>(i);
        desktopIcons.push_back(
            {shared(domain::riceName(icon)),
             shared(iconOf([](const domain::IconsConfig& c) -> const auto& { return c.desktop; }, icon))});
    }
    api.set_icon_desktop_rows(modelOf(std::move(desktopIcons)));
    std::vector<IconRow> folderIcons;
    for (std::size_t i = 0; i < domain::kUserFolderCount; ++i)
    {
        const auto folder = static_cast<domain::UserFolder>(i);
        folderIcons.push_back(
            {shared(domain::riceName(folder)),
             shared(iconOf([](const domain::IconsConfig& c) -> const auto& { return c.folders; }, folder))});
    }
    api.set_icon_folder_rows(modelOf(std::move(folderIcons)));
    std::vector<IconRow> appIcons;
    if (rice.icons)
    {
        for (const auto& [program, file] : rice.icons->apps)
        {
            appIcons.push_back({shared(program), shared(file)});
        }
    }
    api.set_icon_app_rows(modelOf(std::move(appIcons)));
    std::vector<IconRow> typeIcons;
    if (rice.icons)
    {
        for (const auto& [extension, file] : rice.icons->types)
        {
            typeIcons.push_back({shared(extension), shared(file)});
        }
    }
    api.set_icon_type_rows(modelOf(std::move(typeIcons)));
    std::vector<IconRow> machineIcons;
    for (std::size_t i = 0; i < domain::kMachineIconCount; ++i)
    {
        const auto icon = static_cast<domain::MachineIcon>(i);
        machineIcons.push_back(
            {shared(domain::riceName(icon)),
             shared(iconOf([](const domain::IconsConfig& c) -> const auto& { return c.machine; }, icon))});
    }
    api.set_icon_machine_rows(modelOf(std::move(machineIcons)));

    // Start (M11).
    const auto& start = rice.start;
    api.set_start_ours(start && start->menu == domain::StartMenuKind::Ours);
    api.set_start_layout(start && start->layout ? static_cast<int>(*start->layout) + 1 : 0);
    api.set_start_recommended(start && start->recommendations ? (*start->recommendations ? 1 : 2) : 0);
    std::vector<AppChoice> startPinned;
    if (start)
    {
        for (const auto& id : start->pinned)
        {
            const auto app = std::ranges::find_if(
                m_apps, [&](const Json& entry) { return json::stringOr(entry, "id", "") == id; });
            startPinned.push_back({shared(id), shared(app != m_apps.end() ? json::stringOr(*app, "name", id) : id)});
        }
    }
    api.set_start_pinned(modelOf(std::move(startPinned)));

    // Windows' taskbar.
    const auto& taskbar = rice.taskbar;
    api.set_taskbar_alignment(
        taskbar && taskbar->alignment ? (*taskbar->alignment == domain::TaskbarAlignment::Left ? 1 : 2) : 0);
    api.set_taskbar_autohide(taskbar ? triState(taskbar->autohide) : 0);
    api.set_taskbar_task_view(taskbar ? triState(taskbar->taskView) : 0);
    api.set_taskbar_search(taskbar && taskbar->search ? static_cast<int>(*taskbar->search) + 1 : 0);

    showWallpapers();
    showBars();
}

void SettingsApp::showWallpapers()
{
    if (!m_edit)
    {
        return;
    }
    const auto& api = m_ui->global<Api>();
    const domain::Rice& rice = m_edit->loaded.rice;
    std::vector<WallRule> rules;
    std::vector<fs::path> previewFiles;
    if (rice.wallpaper)
    {
        const auto& words = m_ui->global<Words>();
        for (const auto& rule : rice.wallpaper->monitors)
        {
            const std::string match = rule.match.toString();
            const auto found = std::ranges::find(m_matchChoices, match);
            const int index =
                found == m_matchChoices.end() ? 0 : static_cast<int>(found - m_matchChoices.begin());
            slint::SharedString label = words.invoke_match(shared(match));
            if (rule.match.kind == domain::MonitorSelector::Kind::Edid && found != m_matchChoices.end())
            {
                label = *api.get_match_labels()->row_data(static_cast<std::size_t>(index));
            }
            WallRule row{};
            row.match = shared(match);
            row.match_index = index;
            row.match_label = label;
            row.kind = static_cast<int>(rule.kind);
            row.source = shared(rule.source);
            row.fit = std::min(static_cast<int>(rule.fit), 4);
            row.fallback = shared(rule.fallbackImage);
            row.color = rule.color ? shared(rule.color->toHex()) : slint::SharedString();
            row.interval = static_cast<int>(rule.intervalSeconds.value_or(900) / 60);
            row.shuffle = rule.shuffle.value_or(false);
            const auto& e = rule.effects;
            row.brightness = static_cast<float>(e.brightness.value_or(0.0));
            row.contrast = static_cast<float>(e.contrast.value_or(1.0));
            row.saturation = static_cast<float>(e.saturation.value_or(1.0));
            row.hue = static_cast<float>(e.hue.value_or(0.0));
            row.speed = static_cast<float>(e.speed.value_or(1.0));
            row.blur = static_cast<float>(e.blur.value_or(0.0));
            row.parallax = static_cast<float>(e.parallax.value_or(0.0));
            row.zoom = static_cast<float>(e.zoom.value_or(0.0));
            row.vignette = static_cast<float>(e.vignette.value_or(0.0));
            std::vector<ParticleRow> layers;
            for (const auto& layer : rule.particles)
            {
                const auto defaults = video::defaultsFor(layer.kind);
                ParticleRow particles{};
                particles.kind = static_cast<int>(layer.kind);
                particles.color = layer.color ? shared(layer.color->toString()) : slint::SharedString();
                particles.count = static_cast<int>(layer.count.value_or(defaults.count));
                particles.size = static_cast<float>(layer.size.value_or(1.0));
                particles.speed = static_cast<float>(layer.speed.value_or(1.0));
                particles.glow = layer.glow.value_or(defaults.glow);
                layers.push_back(std::move(particles));
            }
            row.particles = modelOf(std::move(layers));
            row.live = !e.empty() || !rule.particles.empty();
            rules.push_back(std::move(row));
            previewFiles.push_back(previewOf(m_edit->folder, rule));
        }
    }
    // No [wallpaper]: nothing to pause, the card is hidden; it shows the defaults, never the
    // previous rice's values.
    const domain::WallpaperPolicy policy = rice.wallpaper ? rice.wallpaper->policy : domain::WallpaperPolicy{};
    api.set_has_wallpaper(rice.wallpaper.has_value());
    api.set_pause_fullscreen(policy.pauseOnFullscreen.value_or(true));
    api.set_pause_battery(policy.pauseOnBattery.value_or(true));
    api.set_pause_covered(policy.pauseWhenCovered.value_or(true));
    api.set_fps_limit(static_cast<int>(policy.fpsLimit.value_or(30)));
    if (api.get_selected_rule() >= static_cast<int>(rules.size()))
    {
        api.set_selected_rule(rules.empty() ? -1 : 0);
    }
    auto ruleModel = modelOf(std::move(rules));
    api.set_rules(ruleModel);

    // The monitors as they sit, fitted into a map four times as wide as high.
    std::vector<MonitorBox> boxes;
    std::vector<domain::MonitorFacts> facts;
    int minX = INT_MAX;
    int minY = INT_MAX;
    int maxX = INT_MIN;
    int maxY = INT_MIN;
    for (const auto& monitor : m_monitors)
    {
        const int left = static_cast<int>(json::intOr(monitor, "left", 0));
        const int top = static_cast<int>(json::intOr(monitor, "top", 0));
        const int width = static_cast<int>(json::intOr(monitor, "width", 0));
        const int height = static_cast<int>(json::intOr(monitor, "height", 0));
        facts.push_back({json::stringOr(monitor, "key", ""), json::boolOr(monitor, "primary", false), left,
                         top, left + width, top + height});
        minX = std::min(minX, left);
        minY = std::min(minY, top);
        maxX = std::max(maxX, left + width);
        maxY = std::max(maxY, top + height);
    }
    std::vector<int> chosen(facts.size(), -1);
    if (rice.wallpaper)
    {
        std::vector<domain::MonitorSelector> selectors;
        for (const auto& rule : rice.wallpaper->monitors)
        {
            selectors.push_back(rule.match);
        }
        chosen = domain::resolveMonitorRules(selectors, facts);
    }
    const double spanX = std::max(1, maxX - minX);
    const double spanY = std::max(1, maxY - minY);
    constexpr double kAspect = 4.0; // the map's width over its height, roughly
    // Fit: the virtual screen keeps its shape inside the map.
    const double shape = spanX / spanY;
    const double fitW = shape >= kAspect ? 1.0 : shape / kAspect;
    const double fitH = shape >= kAspect ? kAspect / shape : 1.0;
    const double offsetX = (1.0 - fitW) / 2.0;
    const double offsetY = (1.0 - fitH) / 2.0;
    for (std::size_t i = 0; i < facts.size(); ++i)
    {
        const auto& monitor = m_monitors[i];
        MonitorBox box{};
        box.key = shared(facts[i].identityKey);
        box.label = shared(json::stringOr(monitor, "name", "?"));
        box.primary = facts[i].primary;
        box.x = static_cast<float>(offsetX + fitW * (facts[i].left - minX) / spanX);
        box.y = static_cast<float>(offsetY + fitH * (facts[i].top - minY) / spanY);
        box.w = static_cast<float>(fitW * (facts[i].right - facts[i].left) / spanX);
        box.h = static_cast<float>(fitH * (facts[i].bottom - facts[i].top) / spanY);
        box.rule = i < chosen.size() ? chosen[i] : -1;
        boxes.push_back(std::move(box));
    }
    auto boxModel = modelOf(std::move(boxes));
    api.set_monitors(boxModel);
    api.set_refreshed(api.get_refreshed() + 1); // the controls show the model again

    // Their pictures, as they come.
    for (std::size_t r = 0; r < previewFiles.size(); ++r)
    {
        const std::weak_ptr<slint::VectorModel<WallRule>> weakRules = ruleModel;
        const std::weak_ptr<slint::VectorModel<MonitorBox>> weakBoxes = boxModel;
        m_thumbnails.request(previewFiles[r], kPreviewWidth, kPreviewHeight,
                             [weakRules, weakBoxes, r](const slint::Image& image) {
                                 if (auto rows = weakRules.lock(); rows && r < rows->row_count())
                                 {
                                     auto row = *rows->row_data(r);
                                     row.preview = image;
                                     row.has_preview = true;
                                     rows->set_row_data(r, row);
                                 }
                                 if (auto boxes = weakBoxes.lock())
                                 {
                                     for (std::size_t b = 0; b < boxes->row_count(); ++b)
                                     {
                                         auto box = *boxes->row_data(b);
                                         if (std::cmp_equal(box.rule, r))
                                         {
                                             box.preview = image;
                                             box.has_preview = true;
                                             boxes->set_row_data(b, box);
                                         }
                                     }
                                 }
                             });
    }
}

void SettingsApp::showBars()
{
    if (!m_edit)
    {
        return;
    }
    const auto& api = m_ui->global<Api>();
    const auto& words = m_ui->global<Words>();
    const domain::Rice& rice = m_edit->loaded.rice;
    const auto labelOf = [&](const domain::MonitorSelector& selector) {
        const std::string match = selector.toString();
        const auto found = std::ranges::find(m_matchChoices, match);
        if (found != m_matchChoices.end() &&
            api.get_match_labels()->row_count() > static_cast<std::size_t>(found - m_matchChoices.begin()))
        {
            return *api.get_match_labels()->row_data(
                static_cast<std::size_t>(found - m_matchChoices.begin()));
        }
        return words.invoke_match(shared(match));
    };
    std::vector<slint::SharedString> names;
    names.reserve(rice.bars.size());
    for (std::size_t i = 0; i < rice.bars.size(); ++i)
    {
        names.push_back(words.invoke_bar_name(
            static_cast<int>(i + 1), static_cast<int>(rice.bars[i].position), labelOf(rice.bars[i].monitor)));
    }
    api.set_bar_names(modelOf(std::move(names)));
    int selected = api.get_selected_bar();
    if (std::cmp_greater_equal(selected, rice.bars.size()))
    {
        selected = rice.bars.empty() ? -1 : 0;
    }
    if (selected < 0 && !rice.bars.empty())
    {
        selected = 0;
    }
    api.set_selected_bar(selected);
    if (selected < 0)
    {
        return;
    }
    // What it looks like: its style fills what it leaves out (M10).
    const domain::BarConfig bar = domain::withStyle(rice.bars[static_cast<std::size_t>(selected)]);
    const auto chips = [&](const std::vector<domain::BarItem>& items) {
        std::vector<WidgetChip> out;
        out.reserve(items.size());
        for (const auto& item : items)
        {
            out.push_back(
                {shared(item.id),
                 item.isPlugin() ? shared(pluginName(item.id)) : words.invoke_widget(shared(item.id)),
                 item.isPlugin()});
        }
        return modelOf(std::move(out));
    };
    BarModel model{};
    model.monitor = shared(bar.monitor.toString());
    const auto found = std::ranges::find(m_matchChoices, bar.monitor.toString());
    model.monitor_index =
        found == m_matchChoices.end() ? 0 : static_cast<int>(found - m_matchChoices.begin());
    model.position = static_cast<int>(bar.position);
    model.mode = static_cast<int>(bar.mode);
    model.thickness = static_cast<int>(bar.thickness);
    model.length_percent = bar.length.percent;
    model.length = static_cast<int>(std::lround(bar.length.value));
    model.margin_top = static_cast<int>(bar.margin[0]);
    model.margin_right = static_cast<int>(bar.margin[1]);
    model.margin_bottom = static_cast<int>(bar.margin[2]);
    model.margin_left = static_cast<int>(bar.margin[3]);
    model.radius = static_cast<int>(bar.radius);
    model.background = static_cast<int>(bar.background.kind);
    model.tint = bar.background.tint ? shared(bar.background.tint->toString()) : slint::SharedString();
    model.opacity = static_cast<int>(std::lround(bar.background.opacity.value_or(1.0) * 100.0));
    model.foreground = bar.foreground ? shared(bar.foreground->toString()) : slint::SharedString();
    model.autohide = bar.autohide;
    model.hide_native = bar.hideNativeTaskbar;
    model.skin = shared(bar.skin);
    model.style = bar.style ? static_cast<int>(*bar.style) + 1 : 0;
    model.length_auto = bar.length.fit;
    model.left = chips(bar.left);
    model.center = chips(bar.center);
    model.right = chips(bar.right);
    api.set_bar(model);
    // Its dock's pinned apps, by the names the app list has (or their ids, before it came).
    domain::BarConfig shownBar = bar;
    const domain::BarItem* dock = dockOf(shownBar, false);
    api.set_bar_has_dock(dock != nullptr);
    std::vector<AppChoice> pinned;
    if (dock != nullptr)
    {
        for (const auto& id : pinnedOf(*dock))
        {
            const auto app = std::ranges::find_if(
                m_apps, [&](const Json& entry) { return json::stringOr(entry, "id", "") == id; });
            pinned.push_back({shared(id), shared(app != m_apps.end() ? json::stringOr(*app, "name", id) : id)});
        }
    }
    api.set_dock_pinned(modelOf(std::move(pinned)));
    api.set_refreshed(api.get_refreshed() + 1); // the controls show the model again
}

const Json* SettingsApp::selectedPlugin() const
{
    if (m_selectedPlugin.empty())
    {
        return nullptr;
    }
    const auto found = std::ranges::find_if(
        m_plugins, [this](const Json& entry) { return json::stringOr(entry, "id", "") == m_selectedPlugin; });
    return found == m_plugins.end() ? nullptr : &*found;
}

void SettingsApp::pluginConfig()
{
    const auto& api = m_ui->global<Api>();
    std::vector<ConfigField> fields;
    if (const Json* selected = selectedPlugin(); m_edit && selected != nullptr)
    {
        const Json& entry = *selected;
        const std::string id = json::stringOr(entry, "id", "");
        const domain::Value* current = nullptr;
        for (const auto& settings : m_edit->loaded.rice.pluginSettings)
        {
            if (settings.id == id)
            {
                current = &settings.config;
            }
        }
        if (auto manifest =
                plugins::readPluginManifest(fs::path(utf8To16Lossy(json::stringOr(entry, "folder", "")))))
        {
            for (const auto& field : manifest->manifest.config.fields)
            {
                const domain::Value* value = current != nullptr ? current->find(field.key) : nullptr;
                const domain::Value& shown = value != nullptr ? *value : field.defaultValue;
                std::string text;
                if (const auto* b = std::get_if<bool>(&shown.data))
                {
                    text = *b ? "true" : "false";
                }
                else if (const auto* i = std::get_if<std::int64_t>(&shown.data))
                {
                    text = std::to_string(*i);
                }
                else if (const auto* d = std::get_if<double>(&shown.data))
                {
                    text = std::format("{}", *d);
                }
                else if (const auto* s = std::get_if<std::string>(&shown.data))
                {
                    text = *s;
                }
                using T = domain::ConfigType;
                const int kind = field.type == T::Int      ? 1
                                 : field.type == T::Float  ? 2
                                 : field.type == T::Bool   ? 3
                                 : field.type == T::Choice ? 4
                                 : field.type == T::Color  ? 5
                                                           : 0;
                std::vector<slint::SharedString> choices;
                choices.reserve(field.choices.size());
                for (const auto& choice : field.choices)
                {
                    choices.push_back(shared(choice));
                }
                fields.push_back({shared(field.key), shared(field.label.empty() ? field.key : field.label),
                                  kind, shared(text), modelOf(std::move(choices)),
                                  shared(field.description)});
            }
        }
    }
    api.set_plugin_config(modelOf(std::move(fields)));
}

void SettingsApp::engineScan()
{
    auto& api = m_ui->global<Api>();
    api.set_engine_status(slint::SharedString(m_ui->global<Words>().invoke_engine_looking()));
    api.set_engine_items(modelOf(std::vector<EngineItem>{}));
    m_engine.clear();
    m_engineScanning = true;
    auto found = std::make_shared<std::vector<settings::EngineWallpaper>>();
    background(
        // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
        [found, folders = m_engineFolders]() -> Reply {
            *found = settings::findEngineWallpapers(folders);
            return Json::object();
        },
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        [this, found](Reply reply) {
            m_engineScanning = false;
            if (!reply)
            {
                failed(reply.error());
                return;
            }
            m_engine = std::move(*found);
            const auto& words = m_ui->global<Words>();
            std::vector<EngineItem> items;
            items.reserve(m_engine.size());
            for (const auto& wallpaper : m_engine)
            {
                EngineItem item{};
                item.title = shared(wallpaper.title);
                item.kind = words.invoke_engine_kind(shared(std::string(settings::toString(wallpaper.kind))));
                item.usable = wallpaper.usable();
                item.note = words.invoke_engine_note(shared(std::string(settings::toString(wallpaper.kind))),
                                                     wallpaper.usable(), !wallpaper.video.empty());
                items.push_back(std::move(item));
            }
            auto model = modelOf(std::move(items));
            auto& api = m_ui->global<Api>();
            api.set_engine_items(model);
            api.set_engine_status(words.invoke_engine_found(static_cast<int>(m_engine.size())));
            for (std::size_t i = 0; i < m_engine.size(); ++i)
            {
                if (m_engine[i].preview.empty())
                {
                    continue;
                }
                const std::weak_ptr<slint::VectorModel<EngineItem>> weak = model;
                m_thumbnails.request(m_engine[i].preview, kPreviewWidth, kPreviewHeight,
                                     [weak, i](const slint::Image& image) {
                                         if (auto rows = weak.lock(); rows && i < rows->row_count())
                                         {
                                             auto row = *rows->row_data(i);
                                             row.preview = image;
                                             row.has_preview = true;
                                             rows->set_row_data(i, row);
                                         }
                                     });
            }
        });
}

void SettingsApp::saveEdited(bool show)
{
    if (!m_edit)
    {
        return;
    }
    Edit& edit = *m_edit;
    if (auto saved = settings::saveRice(edit.folder, edit.loaded.rice); !saved)
    {
        // Not written (it would not read back, or the disk said no): the pages go back to what the
        // file says, and the user hears why.
        log::warn("settings: {} not saved: {}", utf8(edit.folder), saved.error().toString());
        notice(NoticeKind::NotSaved, saved.error().message());
        if (auto reread = settings::loadRice(edit.folder))
        {
            edit.loaded = std::move(*reread);
        }
        showEdited();
        return;
    }
    m_edit->loaded.handWritten = false;
    // What the saved file says now (warnings).
    if (auto reread = settings::loadRice(m_edit->folder))
    {
        m_edit->loaded.diagnostics = std::move(reread->diagnostics);
    }
    if (show)
    {
        showEdited();
    }
}

void SettingsApp::applyFolder(const fs::path& folder)
{
    call(
        "rice.apply", Json{{"path", utf8(folder)}},
        [this](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
                return;
            }
            notice(NoticeKind::Applied, json::stringOr(*reply, "name", ""), true);
            refreshStatus();
            refreshRices();
        },
        true);
}

void SettingsApp::saveSettings()
{
    if (!m_settings)
    {
        return;
    }
    if (auto saved = settings::saveSettings(m_dataDirectory, *m_settings); !saved)
    {
        failed(saved.error());
        return;
    }
    call("settings.reload", Json::object(), [this](Reply reply) {
        if (!reply)
        {
            failed(reply.error());
        }
        refreshStatus();
        refreshSettings();
    });
}

std::string SettingsApp::appName(std::string_view id) const
{
    for (const auto& app : m_apps)
    {
        if (json::stringOr(app, "id", "") == id)
        {
            return json::stringOr(app, "name", std::string(id));
        }
    }
    return std::string(id);
}

void SettingsApp::showApps(std::string_view filter)
{
    constexpr std::size_t kShown = 300;
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        return text;
    };
    const std::string wanted = lower(std::string(filter));
    std::vector<AppChoice> shown;
    for (const auto& app : m_apps)
    {
        const std::string name = json::stringOr(app, "name", "");
        if (wanted.empty() || lower(name).find(wanted) != std::string::npos)
        {
            shown.push_back({shared(json::stringOr(app, "id", "")), shared(name)});
        }
        if (shown.size() >= kShown)
        {
            break;
        }
    }
    m_ui->global<Api>().set_apps_shown(modelOf(std::move(shown)));
}

void SettingsApp::rememberFolder(const fs::path& folder)
{
    auto folders = rememberedFolders();
    if (std::ranges::none_of(folders, [&](const fs::path& f) { return sameFolder(f, folder); }))
    {
        folders.push_back(folder);
    }
    writeRemembered(folders);
}

void SettingsApp::forgetFolder(const fs::path& folder)
{
    auto folders = rememberedFolders();
    std::erase_if(folders, [&](const fs::path& f) { return sameFolder(f, folder); });
    writeRemembered(folders);
}

void SettingsApp::writeRemembered(const std::vector<fs::path>& folders)
{
    Json list = Json::array();
    for (const auto& f : folders)
    {
        list.push_back(utf8(f));
    }
    const std::string text = json::dump(Json{{"folders", std::move(list)}});
    const fs::path file = m_dataDirectory / L"config" / L"settings-app.json";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (auto written = platform::writeFileAtomic(file, std::as_bytes(std::span(text))); !written)
    {
        failed(written.error());
    }
}

std::vector<fs::path> SettingsApp::rememberedFolders() const
{
    std::vector<fs::path> folders;
    auto bytes = platform::readFileLimited(m_dataDirectory / L"config" / L"settings-app.json",
                                           std::uint64_t{256} * 1024);
    if (!bytes)
    {
        return folders;
    }
    auto parsed = json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!parsed)
    {
        return folders;
    }
    for (const auto& folder : json::memberOr(*parsed, "folders", Json::array()))
    {
        if (folder.is_string())
        {
            folders.emplace_back(utf8To16Lossy(folder.get<std::string>()));
        }
    }
    return folders;
}

// ---- what the pages ask ------------------------------------------------------------------------------

void SettingsApp::bind()
{
    const auto& api = m_ui->global<Api>();

    api.on_page_shown([this](Page page) {
        switch (page)
        {
        case Page::Home:
            refreshStatus();
            refreshRices(); // the applied rice's picture and the library strip
            break;
        case Page::Rices:
            refreshRices();
            break;
        case Page::Plugins:
            refreshPlugins();
            break;
        case Page::Diagnostics:
            refreshDiagnostics();
            break;
        case Page::Shortcuts:
        case Page::Tiling:
            refreshSettings();
            break;
        case Page::Wallpapers:
        case Page::Bar:
            refreshMonitors();
            ensureEditing(page);
            break;
        case Page::Appearance:
        case Page::Cursors:
        case Page::Sounds:
        case Page::Icons:
            ensureEditing(page);
            break;
        default:
            break;
        }
    });
    api.on_dismiss_notice([this] { notice(NoticeKind::None); });

    const auto undo = [this] {
        call(
            "action.run", Json{{"name", "rice.undo"}, {"params", Json::object()}},
            [this](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Undone, json::stringOr(*reply, "name", ""));
                refreshStatus();
                refreshRices();
            },
            true);
    };
    api.on_undo_last(undo);
    api.on_confirmed([this, undo](const slint::SharedString& what, const slint::SharedString& arg) {
        const std::string action = text(what);
        if (action == "undo")
        {
            undo();
        }
        else if (action == "restore-original" || action == "snapshot")
        {
            const bool original = action == "restore-original";
            Json params{{"force", false}};
            if (!original)
            {
                params["id"] = std::atoll(text(arg).c_str());
            }
            call(
                original ? "restore.original" : "snapshot.restore", params,
                [this](Reply reply) {
                    if (!reply)
                    {
                        failed(reply.error());
                        return;
                    }
                    std::string notBack;
                    for (const auto& entry : json::memberOr(*reply, "failed", Json::array()))
                    {
                        notBack += std::format(
                            "{}{} ({})", notBack.empty() ? "" : "; ",
                            text(m_ui->global<Words>().invoke_module(shared(json::stringOr(entry, "module", "")))),
                            json::stringOr(entry, "reason", ""));
                    }
                    if (notBack.empty())
                    {
                        notice(NoticeKind::Restored, json::stringOr(*reply, "label", ""));
                    }
                    else
                    {
                        notice(NoticeKind::RestoredPartly, notBack);
                    }
                    refreshStatus();
                    refreshRices();
                },
                true);
        }
        else if (action == "remove-plugin")
        {
            const std::string id = text(arg);
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            call("plugin.remove", Json{{"id", id}}, [this, id](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Removed, pluginName(id));
                refreshPlugins();
            });
        }
        else if (action == "remove-bar" && m_edit)
        {
            const auto index = static_cast<std::size_t>(std::atoi(text(arg).c_str()));
            auto& bars = m_edit->loaded.rice.bars;
            if (index < bars.size())
            {
                bars.erase(bars.begin() + static_cast<std::ptrdiff_t>(index));
                m_ui->global<Api>().set_selected_bar(bars.empty() ? -1 : 0);
                saveEdited();
            }
        }
    });

    // ---- rices -------------------------------------------------------------------------------------
    api.on_apply_rice([this](const slint::SharedString& folder) {
        flushEdits(); // it may be the rice being edited: applied as it was last changed
        applyFolder(pathOf(folder));
    });
    api.on_apply_edited([this] {
        flushEdits();
        if (m_edit)
        {
            applyFolder(m_edit->folder);
        }
    });
    api.on_rename_edited([this](const slint::SharedString& name) {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        auto checked = settings::checkRiceName(text(name));
        if (!checked)
        {
            using P = settings::RiceNameProblem;
            const P why = checked.error();
            const char* key = why == P::Empty         ? "name-empty"
                              : why == P::TooLong     ? "name-long"
                              : why == P::Character   ? "name-character"
                              : why == P::TrailingDot ? "name-dot"
                                                      : "name-device";
            m_ui->global<Api>().set_rename_problem(m_ui->global<Words>().invoke_problem(shared(key)));
            return;
        }
        flushEdits();
        m_ui->global<Api>().set_rename_problem({});
        m_ui->global<Api>().set_renaming(false);
        if (*checked == rice->meta.name)
        {
            return;
        }
        rice->meta.name = *checked;
        saveEdited();
        refreshRices();
    });
    api.on_forget_rice_folder([this](const slint::SharedString& folder) {
        forgetFolder(pathOf(folder));
        refreshRices();
    });
    api.on_edit_rice(
        [this](const slint::SharedString& folder) { startEditing(pathOf(folder), Page::Appearance); });
    api.on_edit_active_rice([this] {
        const std::string current =
            json::stringOr(json::memberOr(m_status, "rice", Json::object()), "current", "");
        if (current.empty())
        {
            return;
        }
        const Page page = m_ui->global<Api>().get_page();
        startEditing(fs::path(utf8To16Lossy(current)),
                     page == Page::Home || page == Page::Rices ? Page::Appearance : page);
    });
    api.on_duplicate_rice([this](const slint::SharedString& folder) {
        flushEdits();
        const fs::path from = pathOf(folder);
        const auto entry = std::ranges::find_if(
            m_riceEntries, [&](const settings::RiceEntry& e) { return sameFolder(e.folder, from); });
        const std::string name = text(m_ui->global<Words>().invoke_copy_name(
            shared(entry != m_riceEntries.end() ? entry->name : utf8(from.filename()))));
        // Every file of the rice is copied (videos too): on the worker thread.
        notice(NoticeKind::Copying, name);
        auto copy = std::make_shared<fs::path>();
        background(
            // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
            [this, from, name, copy]() -> Reply {
                auto made = settings::duplicateRice(m_dataDirectory, from, name);
                if (!made)
                {
                    return std::unexpected(std::move(made.error()));
                }
                *copy = std::move(*made);
                return Json::object();
            },
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this, name](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Created, name);
                refreshRices();
            });
    });
    api.on_open_folder([](const slint::SharedString& folder) { openFolder(pathOf(folder)); });
    api.on_add_rice_folder([this] {
        const auto folder = pickFolder(window());
        if (!folder)
        {
            return;
        }
        std::error_code ec;
        if (!fs::exists(*folder / L"rice.toml", ec))
        {
            refused("no-rice-toml");
            return;
        }
        rememberFolder(*folder);
        refreshRices();
    });
    api.on_import_rice([this] {
        const auto file = pickFile(window(), {{L"Rices", L"*.rice"}});
        if (!file)
        {
            return;
        }
        call(
            "rice.import", Json{{"path", utf8(*file)}},
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Added, json::stringOr(*reply, "name", ""));
                refreshRices();
            },
            true);
    });
    api.on_share_rice([this](const slint::SharedString& folder) {
        flushEdits(); // shared as it was last changed
        const fs::path source = pathOf(folder);
        const std::string name = domain::safeFileName(settings::readEntry(source).name, "rice");
        const auto target = pickSaveFile(window(), {L"Rices", L"*.rice"}, utf8To16Lossy(name) + L".rice");
        if (!target)
        {
            return;
        }
        background(
            // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
            [source, target = *target]() -> Reply {
                auto packed = package::pack(source, package::Kind::Rice, target);
                if (!packed)
                {
                    return std::unexpected(std::move(packed.error()));
                }
                return Json{{"bytes", *packed}};
            },
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this, target = *target](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Shared, utf8(target));
            });
    });
    api.on_new_rice([this] {
        const std::string name = text(m_ui->global<Words>().invoke_new_rice_name());
        auto folder = settings::newRiceFolder(m_dataDirectory, name);
        if (!folder)
        {
            failed(folder.error());
            return;
        }
        domain::Rice rice;
        rice.meta.name = utf8(folder->filename());
        if (auto saved = settings::saveRice(*folder, rice); !saved)
        {
            failed(saved.error());
            return;
        }
        notice(NoticeKind::Created, rice.meta.name);
        refreshRices();
        startEditing(*folder, Page::Appearance);
    });
    api.on_export_current([this] {
        const std::string name = text(m_ui->global<Words>().invoke_my_look_name());
        auto folder = settings::newRiceFolder(m_dataDirectory, name);
        if (!folder)
        {
            failed(folder.error());
            return;
        }
        const std::string riceName = utf8(folder->filename());
        call(
            "rice.export", Json{{"path", utf8(*folder)}, {"name", riceName}},
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this, riceName](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Exported, riceName);
                refreshRices();
            },
            true);
    });

    // ---- appearance ----------------------------------------------------------------------------------
    api.on_appearance_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        const auto& words = m_ui->global<Words>();
        // A field that does not read yet keeps what the rice had, and says why under it; the others
        // are saved (v1.7 saved none of them).
        const domain::AppearanceConfig before = rice->appearance.value_or(domain::AppearanceConfig{});
        domain::AppearanceConfig config;
        using M = domain::ThemeMode;
        constexpr std::array<std::optional<M>, 5> modes = {std::nullopt, M::Light, M::Dark, M::System,
                                                           M::Schedule};
        config.mode = modes[static_cast<std::size_t>(std::clamp(api.get_theme_mode(), 0, 4))];
        slint::SharedString scheduleProblem;
        if (config.mode == M::Schedule)
        {
            auto light = domain::TimeOfDay::parse(text(api.get_light_at()));
            auto dark = domain::TimeOfDay::parse(text(api.get_dark_at()));
            if (!light || !dark || *light == *dark)
            {
                scheduleProblem = words.invoke_problem(shared(!light || !dark ? "time" : "same-times"));
                config.mode = before.mode;
                config.schedule = before.schedule;
            }
            else
            {
                config.schedule = domain::AppearanceSchedule{*light, *dark};
            }
        }
        api.set_schedule_problem(scheduleProblem);
        slint::SharedString accentProblem;
        if (const std::string accent = text(api.get_accent()); !accent.empty())
        {
            if (auto color = domain::Color::parse(accent))
            {
                config.accent = *color;
            }
            else
            {
                accentProblem = words.invoke_problem(shared("color"));
                config.accent = before.accent;
            }
        }
        api.set_accent_problem(accentProblem);
        config.transparency = fromTriState(api.get_transparency());
        config.accentOnTaskbar = fromTriState(api.get_accent_on_taskbar());
        const bool any = config.mode || config.accent || config.transparency || config.accentOnTaskbar;
        rice->appearance = any ? std::optional(config) : std::nullopt;
        saveEdited(false);
    });

    // ---- wallpapers ------------------------------------------------------------------------------------
    // The file (or folder) a wallpaper of `kind` shows, as the user picks it; none when cancelled.
    struct Picked
    {
        fs::path path;
        settings::AssetKind kind = settings::AssetKind::Image;
    };
    const auto pickFor = [this](domain::WallpaperKind kind, bool fallback) -> std::optional<Picked> {
        using K = domain::WallpaperKind;
        std::optional<fs::path> picked;
        settings::AssetKind asset = settings::AssetKind::Image;
        if (fallback || kind == K::Image)
        {
            picked = pickFile(window(), {{L"Images", L"*.jpg;*.jpeg;*.png;*.bmp"}});
        }
        else if (kind == K::Video)
        {
            picked = pickFile(window(), {{L"Videos (MP4, WebM)", L"*.mp4;*.m4v;*.webm"}});
            asset = settings::AssetKind::Video;
        }
        else if (kind == K::Slideshow)
        {
            picked = pickFolder(window());
            asset = settings::AssetKind::Slideshow;
        }
        if (!picked || !m_edit)
        {
            return std::nullopt;
        }
        return Picked{std::move(*picked), asset};
    };
    // A rule by its monitor (the copy takes a while: the user may add or remove rules meanwhile).
    const auto ruleFor = [this](const domain::MonitorSelector& match) -> domain::MonitorWallpaper* {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper)
        {
            return nullptr;
        }
        auto& rules = rice->wallpaper->monitors;
        const auto found = std::ranges::find(rules, match, &domain::MonitorWallpaper::match);
        return found == rules.end() ? nullptr : &*found;
    };
    api.on_rule_field([this, pickFor, ruleFor](int index, const slint::SharedString& field,
                                               const slint::SharedString& value) {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper || index < 0 ||
            static_cast<std::size_t>(index) >= rice->wallpaper->monitors.size())
        {
            return;
        }
        auto& rule = rice->wallpaper->monitors[static_cast<std::size_t>(index)];
        const std::string name = text(field);
        const std::string v = text(value);
        if (name == "match")
        {
            if (auto selector = domain::MonitorSelector::parse(v))
            {
                rule.match = *selector;
            }
        }
        else if (name == "kind")
        {
            const auto kind = static_cast<domain::WallpaperKind>(std::clamp(std::atoi(v.c_str()), 0, 3));
            if (kind == rule.kind)
            {
                return;
            }
            if (kind == domain::WallpaperKind::Color)
            {
                rule.kind = kind;
                rule.source.clear();
                rule.fallbackImage.clear();
                rule.color = rule.color.value_or(domain::Color{30, 30, 46, 255});
                // Nothing to blur, lean or zoom in a color.
                rule.effects.blur.reset();
                rule.effects.parallax.reset();
                rule.effects.zoom.reset();
                rule.effects.speed.reset();
            }
            else
            {
                // A kind with a file needs the file before it is saved (the rice must stay readable):
                // the rule changes once it is copied in.
                auto picked = pickFor(kind, false);
                if (!picked)
                {
                    showWallpapers();
                    return;
                }
                importInto(std::move(picked->path), picked->kind,
                           [this, ruleFor, match = rule.match, kind](const std::string& source) {
                               auto* rule = ruleFor(match);
                               if (rule == nullptr)
                               {
                                   showWallpapers();
                                   return;
                               }
                               rule->kind = kind;
                               rule->source = source;
                               if (kind == domain::WallpaperKind::Video && rule->fit == domain::WallpaperFit::Tile)
                               {
                                   rule->fit = domain::WallpaperFit::Fill;
                               }
                               if (kind != domain::WallpaperKind::Video)
                               {
                                   rule->fallbackImage.clear();
                                   rule->effects.speed.reset();
                               }
                               if (kind == domain::WallpaperKind::Slideshow)
                               {
                                   rule->intervalSeconds = rule->intervalSeconds.value_or(900);
                                   // A slideshow is Windows' own: no effects, no particles.
                                   rule->effects = {};
                                   rule->particles.clear();
                               }
                               saveEdited();
                           });
                return;
            }
        }
        else if (name == "fit")
        {
            rule.fit = static_cast<domain::WallpaperFit>(std::clamp(std::atoi(v.c_str()), 0, 4));
        }
        else if (name == "color")
        {
            auto color = domain::Color::parse(v);
            if (!color)
            {
                refused("color");
                showWallpapers(); // the color it has comes back in the box
                return;
            }
            rule.color = *color;
        }
        else if (name == "interval")
        {
            rule.intervalSeconds = static_cast<std::uint32_t>(std::clamp(std::atoi(v.c_str()), 1, 1440) * 60);
        }
        else if (name == "shuffle")
        {
            rule.shuffle = v == "true";
        }
        saveEdited();
    });
    api.on_rule_pick([this, pickFor, ruleFor](int index, int which) {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper || index < 0 ||
            static_cast<std::size_t>(index) >= rice->wallpaper->monitors.size())
        {
            return;
        }
        const auto& rule = rice->wallpaper->monitors[static_cast<std::size_t>(index)];
        auto picked = pickFor(rule.kind, which == 1);
        if (!picked)
        {
            return;
        }
        importInto(std::move(picked->path), picked->kind,
                   [this, ruleFor, match = rule.match, kind = rule.kind, which](const std::string& stored) {
                       auto* rule = ruleFor(match);
                       if (rule == nullptr || rule->kind != kind)
                       {
                           return; // the rule went, or became another kind, while the file came in
                       }
                       (which == 1 ? rule->fallbackImage : rule->source) = stored;
                       saveEdited();
                   });
    });
    api.on_rule_add([this, pickFor](const slint::SharedString& match) {
        domain::Rice* rice = edited();
        auto selector = domain::MonitorSelector::parse(text(match));
        if (rice == nullptr || !selector)
        {
            return;
        }
        const auto select = [this](const domain::MonitorSelector& which) {
            const auto& rules = edited()->wallpaper->monitors;
            const auto found = std::ranges::find(rules, which, &domain::MonitorWallpaper::match);
            m_ui->global<Api>().set_selected_rule(static_cast<int>(found - rules.begin()));
        };
        if (rice->wallpaper && std::ranges::any_of(rice->wallpaper->monitors,
                                                   [&](const auto& r) { return r.match == *selector; }))
        {
            select(*selector);
            return;
        }
        // A new rule shows an image: picked now, or no rule. It is made once the image is in.
        auto picked = pickFor(domain::WallpaperKind::Image, false);
        if (!picked)
        {
            return;
        }
        importInto(std::move(picked->path), picked->kind,
                   [this, select, selector = *selector](const std::string& source) {
                       domain::Rice* rice = edited();
                       if (rice == nullptr)
                       {
                           return;
                       }
                       auto& rule = settings::wallpaperRule(*rice, selector);
                       rule.source = source;
                       select(selector);
                       matchChoices();
                       saveEdited();
                   });
    });
    api.on_rule_remove([this](int index) {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper || index < 0 ||
            static_cast<std::size_t>(index) >= rice->wallpaper->monitors.size())
        {
            return;
        }
        auto& rules = rice->wallpaper->monitors;
        rules.erase(rules.begin() + index);
        if (rules.empty() && !rice->wallpaper->background)
        {
            rice->wallpaper.reset();
        }
        m_ui->global<Api>().set_selected_rule(-1);
        saveEdited();
    });
    // ---- live wallpapers (M12): effects and particles ----------------------------------------------------
    const auto ruleAt = [this](int index) -> domain::MonitorWallpaper* {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper || index < 0 ||
            static_cast<std::size_t>(index) >= rice->wallpaper->monitors.size())
        {
            return nullptr;
        }
        return &rice->wallpaper->monitors[static_cast<std::size_t>(index)];
    };
    api.on_rule_effect([this, ruleAt](int index, const slint::SharedString& field, float value) {
        auto* rule = ruleAt(index);
        if (rule == nullptr)
        {
            return;
        }
        auto& e = rule->effects;
        // Two decimals are plenty; the neutral value is "leave it" (not written).
        const double rounded = std::round(static_cast<double>(value) * 100.0) / 100.0;
        const auto set = [rounded](std::optional<double>& slot, double neutral, double low, double high) {
            const double v = std::clamp(rounded, low, high);
            if (std::abs(v - neutral) < 0.005)
            {
                slot.reset();
            }
            else
            {
                slot = v;
            }
        };
        const std::string name = text(field);
        if (name == "brightness")
        {
            set(e.brightness, 0.0, -1.0, 1.0);
        }
        else if (name == "contrast")
        {
            set(e.contrast, 1.0, 0.0, 2.0);
        }
        else if (name == "saturation")
        {
            set(e.saturation, 1.0, 0.0, 2.0);
        }
        else if (name == "hue")
        {
            set(e.hue, 0.0, -180.0, 180.0);
        }
        else if (name == "speed" && rule->kind == domain::WallpaperKind::Video)
        {
            set(e.speed, 1.0, 0.25, 4.0);
        }
        else if (name == "blur" && rule->kind != domain::WallpaperKind::Color)
        {
            set(e.blur, 0.0, 0.0, 40.0);
        }
        else if (name == "parallax" && rule->kind != domain::WallpaperKind::Color)
        {
            set(e.parallax, 0.0, 0.0, 1.0);
        }
        else if (name == "zoom" && rule->kind != domain::WallpaperKind::Color)
        {
            set(e.zoom, 0.0, 0.0, 1.0);
        }
        else if (name == "vignette")
        {
            set(e.vignette, 0.0, 0.0, 1.0);
        }
        else
        {
            return;
        }
        // Saved a moment after the last change (a slider moves many times).
        m_barSave.start(slint::TimerMode::SingleShot, kBarSaveDelay, [this] { saveEdited(); });
    });
    api.on_rule_effects_reset([this, ruleAt](int index) {
        if (auto* rule = ruleAt(index); rule != nullptr && !rule->effects.empty())
        {
            rule->effects = {};
            saveEdited();
        }
    });
    api.on_particle_add([this, ruleAt](int index, int kind) {
        auto* rule = ruleAt(index);
        if (rule == nullptr || rule->kind == domain::WallpaperKind::Slideshow ||
            rule->particles.size() >= limits::kWallpaperMaxParticleLayers)
        {
            return;
        }
        domain::ParticleLayer layer;
        layer.kind = static_cast<domain::ParticleKind>(std::clamp(kind, 0, 9));
        rule->particles.push_back(layer);
        saveEdited();
    });
    api.on_particle_field([this, ruleAt](int index, int which, const slint::SharedString& field,
                                         const slint::SharedString& value) {
        auto* rule = ruleAt(index);
        if (rule == nullptr || which < 0 || static_cast<std::size_t>(which) >= rule->particles.size())
        {
            return;
        }
        auto& layer = rule->particles[static_cast<std::size_t>(which)];
        const std::string name = text(field);
        const std::string v = text(value);
        double number = 0.0;
        std::from_chars(v.data(), v.data() + v.size(), number);
        if (name == "kind")
        {
            layer.kind = static_cast<domain::ParticleKind>(std::clamp(std::atoi(v.c_str()), 0, 9));
        }
        else if (name == "color")
        {
            const auto first = v.find_first_not_of(" 	");
            const std::string trimmed =
                first == std::string::npos ? std::string() : v.substr(first, v.find_last_not_of(" 	") - first + 1);
            if (trimmed.empty())
            {
                layer.color.reset();
            }
            else if (trimmed.starts_with("palette."))
            {
                const std::string entry = trimmed.substr(8);
                const domain::Rice* rice = edited();
                if (entry != "accent" && (rice == nullptr || rice->findPalette(entry) == nullptr))
                {
                    refused("palette");
                    return;
                }
                layer.color = domain::ColorRef{entry};
            }
            else
            {
                auto color = domain::Color::parse(trimmed);
                if (!color)
                {
                    failed(color.error());
                    return;
                }
                layer.color = domain::ColorRef{*color};
            }
        }
        else if (name == "count")
        {
            layer.count = static_cast<std::uint32_t>(
                std::clamp(std::atoi(v.c_str()), 1, static_cast<int>(limits::kParticleMaxCount)));
        }
        else if (name == "size")
        {
            const double size = std::clamp(std::round(number * 100.0) / 100.0, 0.25, 4.0);
            layer.size = std::abs(size - 1.0) < 0.005 ? std::nullopt : std::optional(size);
        }
        else if (name == "speed")
        {
            const double speed = std::clamp(std::round(number * 100.0) / 100.0, 0.0, 4.0);
            layer.speed = std::abs(speed - 1.0) < 0.005 ? std::nullopt : std::optional(speed);
        }
        else if (name == "glow")
        {
            layer.glow = v == "true";
        }
        else
        {
            return;
        }
        if (name == "size" || name == "speed")
        {
            m_barSave.start(slint::TimerMode::SingleShot, kBarSaveDelay, [this] { saveEdited(); });
            return;
        }
        saveEdited();
    });
    api.on_particle_remove([this, ruleAt](int index, int which) {
        auto* rule = ruleAt(index);
        if (rule == nullptr || which < 0 || static_cast<std::size_t>(which) >= rule->particles.size())
        {
            return;
        }
        rule->particles.erase(rule->particles.begin() + which);
        saveEdited();
    });
    // ---- Wallpaper Engine (M12): its wallpapers, read where Steam keeps them ------------------------------
    api.on_engine_browse([this](int index) {
        m_engineRule = index;
        auto& api = m_ui->global<Api>();
        api.set_engine_open(true);
        engineScan();
    });
    api.on_engine_add_folder([this] {
        if (auto folder = pickFolder(window()))
        {
            m_engineFolders.push_back(*folder);
            engineScan();
        }
    });
    api.on_engine_close([this] {
        m_ui->global<Api>().set_engine_open(false);
        m_engine.clear();
    });
    api.on_engine_use([this](int item) {
        if (!m_edit || item < 0 || static_cast<std::size_t>(item) >= m_engine.size())
        {
            return;
        }
        const settings::EngineWallpaper chosen = m_engine[static_cast<std::size_t>(item)];
        const fs::path folder = m_edit->folder;
        // The rule by its monitor: rules may come and go while the video is copied.
        const domain::Rice& rice = m_edit->loaded.rice;
        if (!rice.wallpaper || m_engineRule < 0 ||
            static_cast<std::size_t>(m_engineRule) >= rice.wallpaper->monitors.size())
        {
            return;
        }
        const domain::MonitorSelector match = rice.wallpaper->monitors[static_cast<std::size_t>(m_engineRule)].match;
        notice(NoticeKind::Copying, chosen.title);
        // Copied into the rice (a video can be large): on the worker thread.
        auto stored = std::make_shared<std::pair<std::string, std::string>>(); // source, picture
        background(
            // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
            [chosen, folder, stored]() -> Reply {
                const auto picture = [&]() -> Result<std::string> {
                    if (chosen.preview.empty())
                    {
                        return std::string();
                    }
                    fs::path file = chosen.preview;
                    std::error_code ec;
                    std::wstring extension = file.extension().native();
                    std::ranges::transform(extension, extension.begin(),
                                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (extension == L".gif")
                    {
                        // The first frame, as a PNG named after the wallpaper.
                        const fs::path png = fs::temp_directory_path(ec) /
                                             fs::path(utf8To16Lossy(domain::safeFileName(chosen.title, "wallpaper") + ".png"));
                        if (auto saved = savePng(file, png); !saved)
                        {
                            return std::unexpected(std::move(saved.error()));
                        }
                        file = png;
                    }
                    auto imported = settings::importAsset(folder, file, settings::AssetKind::Image);
                    if (file != chosen.preview)
                    {
                        fs::remove(file, ec);
                    }
                    return imported;
                }();
                if (!picture)
                {
                    return std::unexpected(picture.error());
                }
                stored->second = *picture;
                if (!chosen.video.empty())
                {
                    auto video = settings::importAsset(folder, chosen.video, settings::AssetKind::Video);
                    if (!video)
                    {
                        return std::unexpected(std::move(video.error()));
                    }
                    stored->first = *video;
                }
                return Json::object();
            },
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this, chosen, folder, match, stored](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                // Copied into the rice that was edited then: if the user went to another one meanwhile,
                // this one is not changed (v1.7 wrote the first rice's files into the second).
                domain::Rice* rice = edited();
                if (rice == nullptr || !stillEditing(folder) || !rice->wallpaper)
                {
                    notice(NoticeKind::None);
                    return;
                }
                auto& rules = rice->wallpaper->monitors;
                const auto found = std::ranges::find(rules, match, &domain::MonitorWallpaper::match);
                if (found == rules.end())
                {
                    notice(NoticeKind::None);
                    return;
                }
                auto& rule = *found;
                if (!stored->first.empty())
                {
                    rule.kind = domain::WallpaperKind::Video;
                    rule.source = stored->first;
                    rule.fallbackImage = stored->second;
                    if (rule.fit == domain::WallpaperFit::Tile)
                    {
                        rule.fit = domain::WallpaperFit::Fill;
                    }
                }
                else
                {
                    // A scene or a web page plays only inside Wallpaper Engine: its picture, alive.
                    rule.kind = domain::WallpaperKind::Image;
                    rule.source = stored->second;
                    rule.fallbackImage.clear();
                    rule.effects.speed.reset();
                    if (!rule.effects.zoom)
                    {
                        rule.effects.zoom = 0.3;
                    }
                }
                rule.intervalSeconds.reset();
                rule.shuffle.reset();
                // Its files are Wallpaper Engine's and their author's: this rice stays here.
                rice->meta.shareable = false;
                m_ui->global<Api>().set_engine_open(false);
                m_engine.clear();
                notice(NoticeKind::Imported, chosen.title);
                saveEdited();
            });
    });
    api.on_policy_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->wallpaper)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        auto& policy = rice->wallpaper->policy;
        policy.pauseOnFullscreen = api.get_pause_fullscreen();
        policy.pauseOnBattery = api.get_pause_battery();
        policy.pauseWhenCovered = api.get_pause_covered();
        policy.fpsLimit = static_cast<std::uint32_t>(std::clamp(api.get_fps_limit(), 1, 240));
        saveEdited(false);
    });

    // ---- cursors -------------------------------------------------------------------------------------
    api.on_cursors_pick([this] {
        auto folder = pickFolder(window());
        if (edited() == nullptr || !folder)
        {
            return;
        }
        importInto(std::move(*folder), settings::AssetKind::Cursors, [this](const std::string& stored) {
            domain::Rice* rice = edited();
            domain::CursorConfig cursors;
            cursors.source = stored;
            cursors.size = rice->cursors ? rice->cursors->size : std::nullopt;
            std::error_code ec;
            const fs::path copied = settings::assetPath(m_edit->folder, stored);
            if (!fs::exists(copied / L"install.inf", ec))
            {
                cursors.map = guessCursors(copied);
                if (cursors.map.empty())
                {
                    refused("no-cursors");
                    return;
                }
            }
            rice->cursors = std::move(cursors);
            saveEdited();
        });
    });
    api.on_cursors_clear([this] {
        if (domain::Rice* rice = edited())
        {
            rice->cursors.reset();
            saveEdited();
        }
    });
    api.on_cursors_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->cursors)
        {
            return;
        }
        const int index =
            std::clamp(m_ui->global<Api>().get_cursor_size(), 0, static_cast<int>(kCursorSizes.size()) - 1);
        rice->cursors->size =
            index == 0 ? std::nullopt : std::optional(kCursorSizes[static_cast<std::size_t>(index)]);
        saveEdited(false);
    });

    // ---- sounds (M14) ----------------------------------------------------------------------------------
    // The event's assignment in the edited rice, made when `create`; null otherwise.
    const auto soundOf = [this](const slint::SharedString& name, bool create) -> domain::SoundAssignment* {
        domain::Rice* rice = edited();
        const auto event = domain::soundEventFromRiceName(text(name));
        if (rice == nullptr || !event)
        {
            return nullptr;
        }
        if (!rice->sounds)
        {
            if (!create)
            {
                return nullptr;
            }
            rice->sounds.emplace();
        }
        auto& events = rice->sounds->events;
        const auto found = std::ranges::find(events, *event, &domain::SoundAssignment::event);
        if (found != events.end())
        {
            return &*found;
        }
        if (!create)
        {
            return nullptr;
        }
        events.push_back({*event, {}});
        return &events.back();
    };
    // Nothing left of [sounds]: the rice leaves the sounds alone.
    const auto tidySounds = [this] {
        domain::Rice* rice = edited();
        if (rice != nullptr && rice->sounds && !rice->sounds->scheme && rice->sounds->events.empty())
        {
            rice->sounds.reset();
        }
    };
    api.on_sound_pick([this, soundOf](const slint::SharedString& name) {
        auto file = pickFile(window(), {{L"Sounds (.wav)", L"*.wav"}});
        if (!file || edited() == nullptr)
        {
            return;
        }
        importInto(std::move(*file), settings::AssetKind::Sound, [this, soundOf, name](const std::string& stored) {
            if (domain::SoundAssignment* sound = soundOf(name, true))
            {
                sound->file = stored;
                saveEdited();
            }
        });
    });
    api.on_sound_silence([this, soundOf](const slint::SharedString& name) {
        if (domain::SoundAssignment* sound = soundOf(name, true))
        {
            sound->file.clear();
            saveEdited();
        }
    });
    api.on_sound_clear([this, tidySounds](const slint::SharedString& name) {
        domain::Rice* rice = edited();
        const auto event = domain::soundEventFromRiceName(text(name));
        if (rice == nullptr || !rice->sounds || !event)
        {
            return;
        }
        std::erase_if(rice->sounds->events, [&](const domain::SoundAssignment& s) { return s.event == *event; });
        tidySounds();
        saveEdited();
    });
    api.on_sound_play([this, soundOf](const slint::SharedString& name) {
        const domain::SoundAssignment* sound = soundOf(name, false);
        if (sound != nullptr && !sound->file.empty() && m_edit)
        {
            const fs::path file = settings::assetPath(m_edit->folder, sound->file);
            PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        }
    });
    api.on_sounds_edited([this, tidySounds] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const bool silence = m_ui->global<Api>().get_sounds_silence();
        if (silence && !rice->sounds)
        {
            rice->sounds.emplace();
        }
        if (rice->sounds)
        {
            rice->sounds->scheme = silence ? std::optional(domain::SoundScheme::None) : std::nullopt;
        }
        tidySounds();
        saveEdited();
    });

    // ---- other apps' windows (M14) -----------------------------------------------------------------
    api.on_windows_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        domain::WindowsLookConfig look;
        if (const int corners = std::clamp(api.get_windows_corners(), 0, 3); corners > 0)
        {
            look.corners = static_cast<domain::WindowCorners>(corners - 1);
        }
        if (const int backdrop = std::clamp(api.get_windows_backdrop(), 0, 4); backdrop > 0)
        {
            look.backdrop = static_cast<domain::WindowBackdrop>(backdrop - 1);
        }
        look.dark = fromTriState(api.get_windows_dark());
        // A field that does not read (yet: it may be being typed) keeps what the rice had and says
        // why under it; the others are saved (v1.7 saved none of them).
        const domain::WindowsLookConfig before = rice->windows.value_or(domain::WindowsLookConfig{});
        const auto& words = m_ui->global<Words>();
        // "" none; palette.<name> of this rice; #RRGGBB. Returns the problem, "" when it reads.
        const auto colorOf = [&](std::string value, std::optional<domain::ColorRef>& into,
                                 const std::optional<domain::ColorRef>& was) -> slint::SharedString {
            std::erase(value, ' ');
            if (value.empty())
            {
                return {};
            }
            if (value.starts_with("palette.") && value.size() > 8)
            {
                // A name the rice's [palette] does not have would make the rice unreadable.
                const std::string name = value.substr(8);
                if (name != "accent" && rice->findPalette(name) == nullptr)
                {
                    into = was;
                    return words.invoke_problem(shared("palette"));
                }
                into = domain::ColorRef{name};
                return {};
            }
            if (auto color = domain::Color::parse(value))
            {
                into = domain::ColorRef{*color};
                return {};
            }
            into = was;
            return words.invoke_problem(shared("color"));
        };
        slint::SharedString borderProblem;
        if (std::string border = text(api.get_windows_border()); border == "none")
        {
            look.noBorder = true;
        }
        else
        {
            borderProblem = colorOf(std::move(border), look.border, before.border);
            if (!borderProblem.empty())
            {
                look.noBorder = before.noBorder;
            }
        }
        api.set_windows_border_problem(borderProblem);
        api.set_windows_caption_problem(colorOf(text(api.get_windows_caption()), look.caption, before.caption));
        api.set_windows_title_problem(colorOf(text(api.get_windows_title()), look.title, before.title));
        const std::string list = text(api.get_windows_exclude());
        slint::SharedString excludeProblem;
        for (std::size_t start = 0; start <= list.size();)
        {
            const std::size_t comma = std::min(list.find(',', start), list.size());
            std::string name = list.substr(start, comma - start);
            std::erase(name, ' ');
            std::ranges::transform(name, name.begin(),
                                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (!name.empty())
            {
                if (!domain::isValidProgramName(name))
                {
                    excludeProblem = words.invoke_problem(shared("program"));
                    look.exclude = before.exclude;
                    break;
                }
                look.exclude.push_back(std::move(name));
            }
            start = comma + 1;
        }
        api.set_windows_exclude_problem(excludeProblem);
        rice->windows = look.empty() ? std::nullopt : std::optional(std::move(look));
        saveEdited(false);
    });

    // ---- the lock screen (M14) ---------------------------------------------------------------------
    // Nothing left of [lockscreen]: the rice leaves the lock screen alone.
    const auto tidyLock = [this] {
        domain::Rice* rice = edited();
        if (rice != nullptr && rice->lockscreen && rice->lockscreen->image.empty() &&
            !rice->lockscreen->spotlight && !rice->lockscreen->tips)
        {
            rice->lockscreen.reset();
        }
    };
    api.on_lock_pick([this] {
        auto file = pickFile(window(), {{L"Pictures (.jpg, .png)", L"*.jpg;*.jpeg;*.png"}});
        if (!file || edited() == nullptr)
        {
            return;
        }
        importInto(std::move(*file), settings::AssetKind::LockScreen, [this](const std::string& stored) {
            domain::Rice* rice = edited();
            if (!rice->lockscreen)
            {
                rice->lockscreen.emplace();
            }
            rice->lockscreen->image = stored;
            rice->lockscreen->spotlight.reset();
            saveEdited();
        });
    });
    api.on_lock_spotlight_on([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        if (!rice->lockscreen)
        {
            rice->lockscreen.emplace();
        }
        rice->lockscreen->image.clear();
        rice->lockscreen->spotlight = true;
        saveEdited();
    });
    api.on_lock_clear([this, tidyLock] {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->lockscreen)
        {
            return;
        }
        rice->lockscreen->image.clear();
        rice->lockscreen->spotlight.reset();
        tidyLock();
        saveEdited();
    });
    api.on_lock_edited([this, tidyLock] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const int tips = m_ui->global<Api>().get_lock_tips();
        if (tips != 0 && !rice->lockscreen)
        {
            rice->lockscreen.emplace();
        }
        if (rice->lockscreen)
        {
            rice->lockscreen->tips = tips == 0 ? std::nullopt : std::optional(tips == 1);
        }
        tidyLock();
        saveEdited();
    });

    // ---- icons (M14) -----------------------------------------------------------------------------------
    // Sets (file) or removes (empty file) one entry of the edited rice's [icons]; false for an unknown key.
    const auto setIcon = [this](std::string_view kind, std::string key, const std::string& file) {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return false;
        }
        if (!rice->icons)
        {
            if (file.empty())
            {
                return true;
            }
            rice->icons.emplace();
        }
        auto& icons = *rice->icons;
        const auto put = [&file](auto& list, const auto& name) {
            const auto found = std::ranges::find(list, name, [](const auto& entry) { return entry.first; });
            if (file.empty())
            {
                if (found != list.end())
                {
                    list.erase(found);
                }
            }
            else if (found != list.end())
            {
                found->second = file;
            }
            else
            {
                list.emplace_back(name, file);
            }
        };
        bool known = true;
        if (kind == "desktop")
        {
            const auto icon = domain::desktopIconFromRiceName(key);
            known = icon.has_value();
            if (known)
            {
                put(icons.desktop, *icon);
            }
        }
        else if (kind == "folder")
        {
            const auto folder = domain::userFolderFromRiceName(key);
            known = folder.has_value();
            if (known)
            {
                put(icons.folders, *folder);
            }
        }
        else if (kind == "machine")
        {
            const auto machine = domain::machineIconFromRiceName(key);
            known = machine.has_value();
            if (known)
            {
                put(icons.machine, *machine);
            }
        }
        else
        {
            std::ranges::transform(key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (kind == "type")
            {
                if (key.starts_with('.'))
                {
                    key.erase(0, 1);
                }
                known = domain::isValidFileExtension(key);
                if (known)
                {
                    put(icons.types, key);
                }
            }
            else
            {
                known = domain::isValidProgramName(key);
                if (known)
                {
                    put(icons.apps, key);
                }
            }
        }
        // Nothing left of [icons]: the rice leaves the icons alone.
        if (icons.desktop.empty() && icons.folders.empty() && icons.apps.empty() && icons.types.empty() &&
            icons.machine.empty())
        {
            rice->icons.reset();
        }
        return known;
    };
    api.on_icon_pick([this, setIcon](const slint::SharedString& kind, const slint::SharedString& key) {
        if (edited() == nullptr)
        {
            return;
        }
        std::string program = text(key);
        std::ranges::transform(program, program.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (text(kind) == "app" && !domain::isValidProgramName(program))
        {
            refused("program");
            return;
        }
        if (text(kind) == "type" &&
            !domain::isValidFileExtension(program.starts_with('.') ? program.substr(1) : program))
        {
            refused("file-type");
            return;
        }
        auto file = pickFile(window(), {{L"Icons (.ico)", L"*.ico"}});
        if (!file || edited() == nullptr)
        {
            return;
        }
        importInto(std::move(*file), settings::AssetKind::Icon,
                   [this, setIcon, kind = text(kind), key = text(key)](const std::string& stored) {
                       if (setIcon(kind, key, stored))
                       {
                           if (kind == "app")
                           {
                               m_ui->global<Api>().set_icon_new_program({});
                           }
                           if (kind == "type")
                           {
                               m_ui->global<Api>().set_icon_new_type({});
                           }
                           saveEdited();
                       }
                   });
    });
    api.on_icon_clear([this, setIcon](const slint::SharedString& kind, const slint::SharedString& key) {
        if (setIcon(text(kind), text(key), {}))
        {
            saveEdited();
        }
    });

    // ---- Windows' taskbar and our bars -------------------------------------------------------------
    api.on_taskbar_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        domain::TaskbarConfig taskbar;
        const int alignment = api.get_taskbar_alignment();
        if (alignment > 0)
        {
            taskbar.alignment =
                alignment == 1 ? domain::TaskbarAlignment::Left : domain::TaskbarAlignment::Center;
        }
        taskbar.autohide = fromTriState(api.get_taskbar_autohide());
        taskbar.taskView = fromTriState(api.get_taskbar_task_view());
        if (const int search = api.get_taskbar_search(); search > 0)
        {
            taskbar.search = static_cast<domain::TaskbarSearch>(search - 1);
        }
        const bool any = taskbar.alignment || taskbar.autohide || taskbar.taskView || taskbar.search;
        rice->taskbar = any ? std::optional(taskbar) : std::nullopt;
        saveEdited(false);
    });
    api.on_bar_selected([this](int index) {
        m_ui->global<Api>().set_selected_bar(index);
        showBars();
    });
    api.on_bar_add([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr || rice->bars.size() >= limits::kRiceMaxBars)
        {
            return;
        }
        rice->bars.push_back(settings::newBar());
        m_ui->global<Api>().set_selected_bar(static_cast<int>(rice->bars.size()) - 1);
        saveEdited();
    });
    api.on_bar_duplicate([this] {
        domain::Rice* rice = edited();
        const int index = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || index < 0 || static_cast<std::size_t>(index) >= rice->bars.size() ||
            rice->bars.size() >= limits::kRiceMaxBars)
        {
            return;
        }
        rice->bars.push_back(rice->bars[static_cast<std::size_t>(index)]);
        m_ui->global<Api>().set_selected_bar(static_cast<int>(rice->bars.size()) - 1);
        saveEdited();
    });
    api.on_bar_style([this](int style) {
        domain::Rice* rice = edited();
        const int index = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || index < 0 || static_cast<std::size_t>(index) >= rice->bars.size())
        {
            return;
        }
        domain::BarConfig& bar = rice->bars[static_cast<std::size_t>(index)];
        const std::optional<domain::BarStyle> wanted =
            style <= 0 ? std::nullopt : std::optional(static_cast<domain::BarStyle>(std::clamp(style, 1, 5) - 1));
        if (wanted == bar.style)
        {
            return;
        }
        if (wanted)
        {
            // The style, whole: what the bar set before gives way (its monitor and skin stay).
            bar.style = wanted;
            bar.keys.clear();
        }
        else
        {
            // Its own again: it keeps the look it had, now as its own settings.
            bar = domain::withStyle(bar);
            bar.style.reset();
        }
        saveEdited();
    });
    api.on_start_edited([this] {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        auto start = rice->start.value_or(domain::StartConfig{});
        start.menu = api.get_start_ours() ? std::optional(domain::StartMenuKind::Ours) : std::nullopt;
        const int layout = api.get_start_layout();
        start.layout = layout <= 0 ? std::nullopt : std::optional(static_cast<domain::StartLayout>(std::clamp(layout, 1, 3) - 1));
        const int recommended = api.get_start_recommended();
        start.recommendations = recommended == 1 ? std::optional(true) : recommended == 2 ? std::optional(false) : std::nullopt;
        const bool any = start.menu || !start.pinned.empty() || start.layout || start.recommendations;
        rice->start = any ? std::optional(start) : std::nullopt;
        saveEdited();
    });
    api.on_start_unpin([this](int which) {
        domain::Rice* rice = edited();
        if (rice == nullptr || !rice->start || which < 0 || static_cast<std::size_t>(which) >= rice->start->pinned.size())
        {
            return;
        }
        rice->start->pinned.erase(rice->start->pinned.begin() + which);
        saveEdited();
    });
    api.on_dock_unpin([this](int which) {
        domain::Rice* rice = edited();
        const int selected = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || selected < 0 || static_cast<std::size_t>(selected) >= rice->bars.size())
        {
            return;
        }
        if (auto* dock = dockOf(rice->bars[static_cast<std::size_t>(selected)], true))
        {
            auto ids = pinnedOf(*dock);
            if (which >= 0 && static_cast<std::size_t>(which) < ids.size())
            {
                ids.erase(ids.begin() + which);
                setPinned(*dock, ids);
                saveEdited();
            }
        }
    });
    api.on_bar_remove([this] {
        const int index = m_ui->global<Api>().get_selected_bar();
        m_ui->global<Api>().set_confirm_what("remove-bar");
        m_ui->global<Api>().set_confirm_arg(shared(std::to_string(index)));
    });
    api.on_bar_edited([this] {
        domain::Rice* rice = edited();
        const int index = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || index < 0 || static_cast<std::size_t>(index) >= rice->bars.size())
        {
            return;
        }
        domain::BarConfig& bar = rice->bars[static_cast<std::size_t>(index)];
        const domain::BarConfig before = bar; // what a refused change goes back to
        // Compared with what it shows (its style filled in): only what the user changed is its own.
        const domain::BarConfig shown = domain::withStyle(bar);
        const BarModel model = m_ui->global<Api>().get_bar();
        const auto changed = [&bar](std::string_view key, bool differs) {
            if (differs)
            {
                settings::markKey(bar, key);
            }
            return differs;
        };
        if (auto monitor = domain::MonitorSelector::parse(text(model.monitor));
            monitor && *monitor != bar.monitor)
        {
            bar.monitor = *monitor;
        }
        if (changed("position", static_cast<int>(shown.position) != model.position))
        {
            bar.position = static_cast<domain::BarPosition>(std::clamp(model.position, 0, 3));
        }
        if (changed("mode", static_cast<int>(shown.mode) != model.mode))
        {
            bar.mode = static_cast<domain::BarMode>(std::clamp(model.mode, 0, 1));
        }
        if (changed("thickness", std::cmp_not_equal(shown.thickness, model.thickness)))
        {
            bar.thickness = static_cast<std::uint32_t>(std::clamp(model.thickness, 16, 512));
        }
        const domain::BarLength length{model.length_percent,
                                       model.length_percent ? std::clamp(model.length, 1, 100) * 1.0
                                                            : std::clamp(model.length, 16, 16384) * 1.0,
                                       model.length_auto};
        if (changed("length", length.fit != shown.length.fit ||
                                  (!length.fit && (length.percent != shown.length.percent ||
                                                   length.value != shown.length.value))))
        {
            bar.length = length;
        }
        const std::array<std::uint32_t, 4> margin = {
            static_cast<std::uint32_t>(std::clamp(model.margin_top, 0, 1024)),
            static_cast<std::uint32_t>(std::clamp(model.margin_right, 0, 1024)),
            static_cast<std::uint32_t>(std::clamp(model.margin_bottom, 0, 1024)),
            static_cast<std::uint32_t>(std::clamp(model.margin_left, 0, 1024))};
        if (changed("margin", margin != shown.margin))
        {
            bar.margin = margin;
        }
        if (changed("radius", std::cmp_not_equal(shown.radius, model.radius)))
        {
            bar.radius = static_cast<std::uint32_t>(std::clamp(model.radius, 0, 256));
        }
        // "" none, palette.<name>, #RRGGBB; what does not read keeps the color it had (v1.7 erased it).
        bool unreadable = false;
        const auto colorOf = [&unreadable](std::string value, const std::optional<domain::ColorRef>& was) {
            std::erase(value, ' ');
            if (value.empty())
            {
                return std::optional<domain::ColorRef>();
            }
            if (value.starts_with("palette.") && value.size() > 8)
            {
                return std::optional(domain::ColorRef{value.substr(8)});
            }
            if (auto color = domain::Color::parse(value))
            {
                return std::optional(domain::ColorRef{*color});
            }
            unreadable = true;
            return was;
        };
        domain::BarBackground background;
        background.kind = static_cast<domain::BarBackground::Kind>(std::clamp(model.background, 0, 3));
        background.tint = colorOf(text(model.tint), shown.background.tint);
        background.opacity = std::clamp(model.opacity, 0, 100) / 100.0;
        if (changed("background", background.kind != shown.background.kind ||
                                      background.tint != shown.background.tint ||
                                      std::lround(background.opacity.value_or(1.0) * 100) !=
                                          std::lround(shown.background.opacity.value_or(1.0) * 100)))
        {
            bar.background = background;
        }
        const std::optional<domain::ColorRef> foreground = colorOf(text(model.foreground), shown.foreground);
        if (changed("foreground", foreground != shown.foreground))
        {
            bar.foreground = foreground;
        }
        if (changed("autohide", shown.autohide != model.autohide))
        {
            bar.autohide = model.autohide;
        }
        if (changed("hide_native_taskbar", shown.hideNativeTaskbar != model.hide_native))
        {
            bar.hideNativeTaskbar = model.hide_native;
        }
        // A palette name the rice does not have would make it unreadable: not saved.
        const auto known = [rice](const std::optional<domain::ColorRef>& ref) {
            return !ref || !ref->isPalette() || std::get<std::string>(ref->value) == "accent" ||
                   rice->findPalette(std::get<std::string>(ref->value)) != nullptr;
        };
        if (!known(bar.background.tint) || !known(bar.foreground))
        {
            // Not kept: the bar goes back to what it was (other changes still waiting to be saved stay).
            refused("palette");
            bar = before;
            showBars();
            return;
        }
        if (unreadable)
        {
            refused("color");
        }
        // Saved a moment after the last change (a slider moves many times).
        m_barSave.start(slint::TimerMode::SingleShot, kBarSaveDelay, [this] { saveEdited(); });
    });
    const auto zone = [](int value) { return static_cast<settings::Zone>(std::clamp(value, 0, 2)); };
    api.on_bar_move_item([this, zone](int from, int index, int to, int toIndex) {
        domain::Rice* rice = edited();
        const int bar = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || bar < 0 || static_cast<std::size_t>(bar) >= rice->bars.size() || index < 0 ||
            toIndex < 0)
        {
            return;
        }
        settings::moveItem(rice->bars[static_cast<std::size_t>(bar)], zone(from),
                           static_cast<std::size_t>(index), zone(to), static_cast<std::size_t>(toIndex));
        saveEdited();
    });
    api.on_bar_add_item([this, zone](int to, const slint::SharedString& id) {
        domain::Rice* rice = edited();
        const int bar = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || bar < 0 || static_cast<std::size_t>(bar) >= rice->bars.size())
        {
            return;
        }
        const std::string widget = text(id);
        settings::addItem(rice->bars[static_cast<std::size_t>(bar)], zone(to), widget);
        // A plugin's widget: the rice asks for the plugin.
        if (widget.find('.') != std::string::npos &&
            std::ranges::none_of(rice->pluginRequirements, [&](const auto& r) { return r.id == widget; }))
        {
            rice->pluginRequirements.push_back({widget, {}});
        }
        saveEdited();
    });
    api.on_bar_remove_item([this, zone](int from, int index) {
        domain::Rice* rice = edited();
        const int bar = m_ui->global<Api>().get_selected_bar();
        if (rice == nullptr || bar < 0 || static_cast<std::size_t>(bar) >= rice->bars.size() || index < 0)
        {
            return;
        }
        settings::removeItem(rice->bars[static_cast<std::size_t>(bar)], zone(from),
                             static_cast<std::size_t>(index));
        saveEdited();
    });

    // ---- plugins -------------------------------------------------------------------------------------
    api.on_plugins_refresh([this] { refreshPlugins(); });
    api.on_plugin_selected([this](const slint::SharedString& id) {
        m_selectedPlugin = text(id);
        const auto found = std::ranges::find_if(
            m_plugins, [this](const Json& entry) { return json::stringOr(entry, "id", "") == m_selectedPlugin; });
        if (found == m_plugins.end())
        {
            m_selectedPlugin.clear();
        }
        m_ui->global<Api>().set_selected_plugin(found == m_plugins.end() ? -1
                                                                          : static_cast<int>(found - m_plugins.begin()));
        pluginConfig();
    });
    api.on_plugin_install([this] {
        const auto folder = pickFolder(window());
        if (!folder)
        {
            return;
        }
        call("plugin.install", Json{{"path", utf8(*folder)}, {"approve", false}}, [this](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
                return;
            }
            notice(NoticeKind::Installed, json::stringOr(*reply, "name", json::stringOr(*reply, "id", "")));
            refreshPlugins();
        });
    });
    api.on_plugin_install_file([this] {
        const auto file = pickFile(window(), {{L"Plugins", L"*.rmwp"}});
        if (!file)
        {
            return;
        }
        // Installed unapproved: its card then shows what it asks for, and "Approve".
        call(
            "plugin.install", Json{{"path", utf8(*file)}, {"approve", false}},
            [this](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Installed,
                       json::stringOr(*reply, "name", json::stringOr(*reply, "id", "")));
                refreshPlugins();
            },
            true);
    });
    api.on_plugin_approve([this](const slint::SharedString& id) {
        const std::string plugin = text(id);
        // What its card shows is what is approved: the core refuses when plugin.toml asks for more
        // now (it changed since the list was read).
        const auto entry = std::ranges::find_if(
            m_plugins, [&](const Json& e) { return json::stringOr(e, "id", "") == plugin; });
        if (entry == m_plugins.end() ||
            !plugins::permissionsFromJson(json::memberOr(*entry, "permissions", Json::object())))
        {
            refreshPlugins();
            return;
        }
        const Json shown = json::memberOr(*entry, "permissions", Json::object());
        // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
        call("plugin.approve", Json{{"id", plugin}, {"permissions", shown}}, [this, plugin](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
                refreshPlugins(); // what it asks for now
                return;
            }
            notice(NoticeKind::Approved, pluginName(plugin));
            refreshPlugins();
        });
    });
    api.on_plugin_enable([this](const slint::SharedString& id, bool on) {
        call(on ? "plugin.enable" : "plugin.disable", Json{{"id", text(id)}}, [this](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
            }
            refreshPlugins();
        });
    });
    api.on_plugin_remove([this](const slint::SharedString& id) {
        m_ui->global<Api>().set_confirm_what("remove-plugin");
        m_ui->global<Api>().set_confirm_arg(id);
    });
    api.on_plugin_use([this](const slint::SharedString& id, bool use) {
        domain::Rice* rice = edited();
        if (rice == nullptr)
        {
            return;
        }
        const std::string plugin = text(id);
        auto& requirements = rice->pluginRequirements;
        std::erase_if(requirements, [&](const auto& r) { return r.id == plugin; });
        if (use)
        {
            requirements.push_back({plugin, {}});
        }
        saveEdited();
        refreshPlugins();
    });
    api.on_plugin_config_edited([this](const slint::SharedString& key, const slint::SharedString& value) {
        domain::Rice* rice = edited();
        const Json* selected = selectedPlugin();
        if (rice == nullptr || selected == nullptr)
        {
            return;
        }
        const Json entry = *selected; // a copy: refreshing the list below replaces m_plugins
        auto manifest =
            plugins::readPluginManifest(fs::path(utf8To16Lossy(json::stringOr(entry, "folder", ""))));
        if (!manifest)
        {
            failed(manifest.error());
            return;
        }
        const auto* field = manifest->manifest.config.find(text(key));
        if (field == nullptr)
        {
            return;
        }
        const std::string v = text(value);
        domain::Value parsed;
        using T = domain::ConfigType;
        const auto bad = [this] {
            refused("value");
            pluginConfig(); // the box shows the value the rice has again
        };
        switch (field->type)
        {
        case T::Bool:
            parsed.data = v == "true";
            break;
        case T::Int: {
            std::int64_t number = 0;
            const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), number);
            if (ec != std::errc{} || end != v.data() + v.size() ||
                (field->min && static_cast<double>(number) < *field->min) ||
                (field->max && static_cast<double>(number) > *field->max))
            {
                bad();
                return;
            }
            parsed.data = number;
            break;
        }
        case T::Float: {
            double number = 0;
            const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), number);
            // "nan" and "inf" read as numbers, and NaN passes every min and max.
            if (ec != std::errc{} || end != v.data() + v.size() || !std::isfinite(number) ||
                (field->min && number < *field->min) || (field->max && number > *field->max))
            {
                bad();
                return;
            }
            parsed.data = number;
            break;
        }
        case T::Color:
            if (!v.starts_with("palette.") && !domain::Color::parse(v))
            {
                bad();
                return;
            }
            parsed.data = v;
            break;
        case T::Choice:
            if (std::ranges::find(field->choices, v) == field->choices.end())
            {
                bad();
                return;
            }
            parsed.data = v;
            break;
        case T::String:
            if (v.size() > field->maxLength)
            {
                bad();
                return;
            }
            parsed.data = v;
            break;
        }
        const std::string id = json::stringOr(entry, "id", "");
        auto found = std::ranges::find(rice->pluginSettings, id, &domain::PluginSettings::id);
        if (found == rice->pluginSettings.end())
        {
            rice->pluginSettings.push_back({id, domain::Value{domain::Value::Table{}}});
            found = rice->pluginSettings.end() - 1;
        }
        if (found->config.table() == nullptr)
        {
            found->config.data = domain::Value::Table{};
        }
        auto& table = std::get<domain::Value::Table>(found->config.data);
        const auto member = std::ranges::find(table, field->key, &domain::ValueMember::key);
        if (member != table.end())
        {
            member->value = std::move(parsed);
        }
        else
        {
            table.push_back({field->key, std::move(parsed)});
            std::ranges::sort(table, {}, &domain::ValueMember::key); // as TOML reads them back
        }
        saveEdited(false);
        pluginConfig();
    });

    // ---- hotkeys and rules ---------------------------------------------------------------------------
    api.on_action_index([this](const slint::SharedString& name) -> int {
        const auto actions = m_ui->global<Api>().get_actions();
        for (std::size_t i = 0; i < actions->row_count(); ++i)
        {
            if (actions->row_data(i)->name == name)
            {
                return static_cast<int>(i);
            }
        }
        return -1; // an action nobody offers now (a plugin's, removed): no other action is shown instead
    });
    api.on_match_index([this](const slint::SharedString& selector) -> int {
        const auto found = std::ranges::find(m_matchChoices, text(selector));
        return found == m_matchChoices.end() ? 0 : static_cast<int>(found - m_matchChoices.begin());
    });
    const auto askRice = [this]() -> std::optional<std::string> {
        const auto folder = pickFolder(window());
        std::error_code ec;
        if (!folder || !fs::exists(*folder / L"rice.toml", ec))
        {
            return std::nullopt;
        }
        return utf8(*folder);
    };
    const auto actionFor = [askRice](domain::ActionCall& call, const std::string& name) {
        if (name == call.name)
        {
            return true;
        }
        if (name == "rice.apply" && call.ricePath.empty())
        {
            auto rice = askRice();
            if (!rice)
            {
                return false; // rice.apply without a rice would not read
            }
            call.ricePath = *rice;
        }
        if (name == "launch.app" && call.app.empty())
        {
            return false; // the app is picked first (pick-app-for), then it is saved
        }
        call.name = name;
        if (name != "rice.apply")
        {
            call.ricePath.clear();
        }
        if (name != "launch.app")
        {
            call.app.clear();
        }
        return true;
    };
    const auto pickAppFirst = [this](int kind, int index, const slint::SharedString& action) {
        if (std::string_view(action) == "launch.app")
        {
            m_ui->global<Api>().invoke_pick_app_for(kind, index);
            return true;
        }
        return false;
    };
    // A hotkey another one has, or a tiling key while tiling is on, made settings.toml one the Tiling
    // page could no longer save (v1.7): not saved; the row says why.
    const auto hotkeyProblem = [this](const domain::Settings& next, std::size_t index) -> std::optional<std::string> {
        const domain::KeyChord& keys = next.hotkeys[index].keys;
        for (std::size_t i = 0; i < next.hotkeys.size(); ++i)
        {
            if (i != index && next.hotkeys[i].keys == keys)
            {
                return text(m_ui->global<Words>().invoke_taken(shared(keys.toString())));
            }
        }
        auto problem = domain::tilingProblem(next);
        return problem != domain::tilingProblem(*m_settings) ? problem : std::nullopt;
    };
    api.on_hotkey_add([this, hotkeyProblem] {
        if (!m_settings)
        {
            return;
        }
        for (int digit = 1; digit <= 9; ++digit)
        {
            auto keys = domain::KeyChord::parse(std::format("Ctrl+Alt+Win+{}", digit));
            if (!keys)
            {
                continue;
            }
            domain::Settings next = *m_settings;
            next.hotkeys.push_back({*keys, {"appearance.toggle", {}, {}}});
            if (!hotkeyProblem(next, next.hotkeys.size() - 1))
            {
                *m_settings = std::move(next);
                saveSettings();
                return;
            }
        }
    });
    api.on_hotkey_edited([this, actionFor, pickAppFirst, hotkeyProblem](int index, const HotkeyRow& row) {
        if (!m_settings || index < 0 || static_cast<std::size_t>(index) >= m_settings->hotkeys.size())
        {
            return;
        }
        const auto showProblem = [this, index, &row](const std::string& problem) {
            auto rows = m_ui->global<Api>().get_hotkeys();
            auto shown = *rows->row_data(static_cast<std::size_t>(index));
            shown.keys = row.keys;
            shown.problem = shared(problem);
            std::static_pointer_cast<slint::VectorModel<HotkeyRow>>(rows)->set_row_data(
                static_cast<std::size_t>(index), shown);
        };
        auto keys = domain::KeyChord::parse(text(row.keys));
        if (!keys)
        {
            showProblem(keys.error().message());
            return;
        }
        domain::Settings next = *m_settings;
        next.hotkeys[static_cast<std::size_t>(index)].keys = *keys;
        if (const auto problem = hotkeyProblem(next, static_cast<std::size_t>(index)))
        {
            showProblem(*problem);
            return;
        }
        auto& hotkey = m_settings->hotkeys[static_cast<std::size_t>(index)];
        hotkey.keys = *keys;
        if (hotkey.action.name != "launch.app" && pickAppFirst(0, index, row.action))
        {
            return;
        }
        if (!actionFor(hotkey.action, text(row.action)))
        {
            refreshSettings();
            return;
        }
        saveSettings();
    });
    api.on_hotkey_remove([this](int index) {
        if (m_settings && index >= 0 && static_cast<std::size_t>(index) < m_settings->hotkeys.size())
        {
            m_settings->hotkeys.erase(m_settings->hotkeys.begin() + index);
            saveSettings();
        }
    });
    api.on_rule_list_add([this] {
        if (!m_settings)
        {
            return;
        }
        domain::Rule rule;
        rule.trigger = domain::RuleTrigger::Time;
        rule.at = domain::TimeOfDay{19, 0};
        rule.action = {"appearance.dark", {}, {}};
        m_settings->rules.push_back(std::move(rule));
        saveSettings();
    });
    api.on_rule_list_edited([this, actionFor, pickAppFirst](int index, const RuleRow& row) {
        if (!m_settings || index < 0 || static_cast<std::size_t>(index) >= m_settings->rules.size())
        {
            return;
        }
        auto& rule = m_settings->rules[static_cast<std::size_t>(index)];
        rule.trigger = static_cast<domain::RuleTrigger>(std::clamp(row.when, 0, 3));
        rule.at.reset();
        rule.monitor.reset();
        if (rule.trigger == domain::RuleTrigger::Time)
        {
            auto at = domain::TimeOfDay::parse(text(row.at));
            if (!at)
            {
                failed(at.error());
                refreshSettings();
                return;
            }
            rule.at = *at;
        }
        if (rule.trigger == domain::RuleTrigger::MonitorConnected)
        {
            auto monitor = domain::MonitorSelector::parse(text(row.monitor));
            rule.monitor = monitor ? *monitor : *domain::MonitorSelector::parse("all");
        }
        if (rule.action.name != "launch.app" && pickAppFirst(1, index, row.action))
        {
            return;
        }
        if (!actionFor(rule.action, text(row.action)))
        {
            refreshSettings();
            return;
        }
        saveSettings();
    });
    api.on_rule_list_remove([this](int index) {
        if (m_settings && index >= 0 && static_cast<std::size_t>(index) < m_settings->rules.size())
        {
            m_settings->rules.erase(m_settings->rules.begin() + index);
            saveSettings();
        }
    });
    api.on_pick_rice_for([this, askRice](int kind, int index) {
        if (!m_settings || index < 0)
        {
            return;
        }
        auto rice = askRice();
        if (!rice)
        {
            return;
        }
        if (kind == 0 && static_cast<std::size_t>(index) < m_settings->hotkeys.size())
        {
            m_settings->hotkeys[static_cast<std::size_t>(index)].action.ricePath = *rice;
        }
        else if (kind == 1 && static_cast<std::size_t>(index) < m_settings->rules.size())
        {
            m_settings->rules[static_cast<std::size_t>(index)].action.ricePath = *rice;
        }
        saveSettings();
    });

    api.on_pick_app_for([this](int kind, int index) {
        m_appFor = std::pair{kind, index};
        m_ui->global<Api>().set_picking_app(true);
        if (!m_apps.empty())
        {
            showApps({});
            return;
        }
        call("apps.list", Json::object(), [this](Reply reply) {
            if (!reply || !reply->is_array())
            {
                m_ui->global<Api>().set_picking_app(false);
                failed(reply ? Error(ErrorCode::InvalidData, "no list of apps") : reply.error());
                return;
            }
            m_apps = std::move(*reply);
            showApps({});
        });
    });
    api.on_app_filter_edited(
        [this](const slint::SharedString& filter) { showApps(std::string_view(filter)); });
    api.on_app_picked([this](const slint::SharedString& id) {
        m_ui->global<Api>().set_picking_app(false);
        const auto target = std::exchange(m_appFor, std::nullopt);
        if (id.empty() || !target || (target->first < 2 && !m_settings))
        {
            if (!target || target->first < 2)
            {
                refreshSettings(); // what the window showed goes back to the file's
            }
            return;
        }
        const auto [kind, index] = *target;
        if (kind == 3)
        {
            // Pinned to Start.
            if (domain::Rice* rice = edited())
            {
                auto& start = rice->start ? *rice->start : rice->start.emplace();
                if (std::ranges::find(start.pinned, text(id)) == start.pinned.end() &&
                    start.pinned.size() < limits::kStartMaxPinned)
                {
                    start.pinned.push_back(text(id));
                    saveEdited();
                }
            }
            return;
        }
        if (kind == 2)
        {
            // Pinned to the selected bar's dock.
            domain::Rice* rice = edited();
            const int selected = m_ui->global<Api>().get_selected_bar();
            if (rice != nullptr && selected >= 0 && static_cast<std::size_t>(selected) < rice->bars.size())
            {
                if (auto* dock = dockOf(rice->bars[static_cast<std::size_t>(selected)], true))
                {
                    auto ids = pinnedOf(*dock);
                    if (std::ranges::find(ids, text(id)) == ids.end() && ids.size() < 32)
                    {
                        ids.push_back(text(id));
                        setPinned(*dock, ids);
                        saveEdited();
                    }
                }
            }
            return;
        }
        domain::ActionCall* action = nullptr;
        if (kind == 0 && index >= 0 && static_cast<std::size_t>(index) < m_settings->hotkeys.size())
        {
            action = &m_settings->hotkeys[static_cast<std::size_t>(index)].action;
        }
        else if (kind == 1 && index >= 0 && static_cast<std::size_t>(index) < m_settings->rules.size())
        {
            action = &m_settings->rules[static_cast<std::size_t>(index)].action;
        }
        if (action != nullptr)
        {
            *action = {"launch.app", {}, text(id)};
            saveSettings();
        }
    });

    // ---- diagnostics -----------------------------------------------------------------------------------
    api.on_diagnostics_refresh([this] { refreshDiagnostics(); });
    api.on_open_logs([this] { openFolder(m_dataDirectory / L"logs"); });
    api.on_support_bundle([this] {
        // Every log is copied: on the worker thread.
        auto folder = std::make_shared<fs::path>();
        background(
            // NOLINTNEXTLINE(bugprone-exception-escape): runs on the worker thread, like its core calls
            [this, folder,
             state = json::dumpPretty(Json{{"status", m_status}, {"plugins", m_plugins}, {"monitors", m_monitors}})]()
                -> Reply {
                auto made = makeSupportBundle(m_dataDirectory, state);
                if (!made)
                {
                    return std::unexpected(std::move(made.error()));
                }
                *folder = std::move(*made);
                return Json::object();
            },
            // NOLINTNEXTLINE(bugprone-exception-escape): runs from Slint's event loop, like any callback
            [this, folder](Reply reply) {
                if (!reply)
                {
                    failed(reply.error());
                    return;
                }
                notice(NoticeKind::Bundle, utf8(*folder));
                openFolder(*folder);
            });
    });

    // ---- settings ----------------------------------------------------------------------------------------
    api.on_autostart_edited([this] {
        call("autostart.set", Json{{"enabled", m_ui->global<Api>().get_autostart()}}, [this](Reply reply) {
            if (!reply)
            {
                failed(reply.error());
                return;
            }
            m_ui->global<Api>().set_autostart(json::boolOr(*reply, "enabled", false));
        });
    });
    bindTiling();
    api.on_settings_edited([this] {
        const auto& api = m_ui->global<Api>();
        applyLanguage();
        if (!m_settings)
        {
            return;
        }
        m_settings->keepInTray = api.get_keep_in_tray();
        m_settings->startWinKey = api.get_start_win_key();
        m_settings->machineIcons = api.get_machine_icons();
        const int language = api.get_language();
        m_settings->language = language == 1   ? std::string(domain::kLanguageEnglish)
                               : language == 2 ? std::string(domain::kLanguagePortuguese)
                                               : std::string();
        saveSettings();
    });
}

bool SettingsApp::saveTiling(domain::Settings next)
{
    const auto& api = m_ui->global<Api>();
    if (const auto problem = domain::tilingProblem(next))
    {
        api.set_tiling_problem(shared(*problem));
        return false;
    }
    api.set_tiling_problem(shared(""));
    *m_settings = std::move(next);
    saveSettings();
    return true;
}

void SettingsApp::bindTiling()
{
    const auto& api = m_ui->global<Api>();
    api.on_tiling_edited([this] {
        if (!m_settings)
        {
            return;
        }
        const auto& api = m_ui->global<Api>();
        domain::Settings next = *m_settings;
        domain::TilingSettings& tiling = next.tiling;
        tiling.enabled = api.get_tiling_enabled();
        tiling.mode = api.get_tiling_mode() == 1 ? domain::TilingMode::Mac : domain::TilingMode::Linux;
        tiling.layout = static_cast<domain::TilingLayout>(std::clamp(api.get_tiling_layout(), 0, 4));
        tiling.workspaces = std::clamp(api.get_tiling_workspaces(), 1, domain::kMaxWorkspaces);
        tiling.gap = std::clamp(api.get_tiling_gap(), 0, 64);
        tiling.outerGap = std::clamp(api.get_tiling_outer_gap(), 0, 64);
        tiling.masterPercent = std::clamp(api.get_tiling_master(), 10, 90);
        tiling.border = api.get_tiling_border();
        tiling.runAsAdmin = api.get_tiling_admin();
        tiling.animations = api.get_tiling_animations();
        const std::string color = text(api.get_tiling_border_color());
        if (color.empty())
        {
            tiling.borderColor.reset();
        }
        else if (auto parsed = domain::Color::parse(color))
        {
            tiling.borderColor = *parsed;
        }
        else
        {
            api.set_tiling_problem(shared(parsed.error().message()));
            return;
        }
        saveTiling(std::move(next));
    });
    api.on_tiling_key_edited([this](int index, const slint::SharedString& value) {
        if (!m_settings || index < 0 || std::cmp_greater_equal(index, domain::kTilingCommandCount))
        {
            return;
        }
        const auto command = static_cast<domain::TilingCommand>(index);
        const auto showProblem = [this, index, &value](const std::string& problem) {
            auto rows = m_ui->global<Api>().get_tiling_keys();
            auto shown = *rows->row_data(static_cast<std::size_t>(index));
            shown.keys = value;
            shown.problem = shared(problem);
            std::static_pointer_cast<slint::VectorModel<TilingKeyRow>>(rows)->set_row_data(
                static_cast<std::size_t>(index), shown);
        };
        domain::Settings next = *m_settings;
        std::erase_if(next.tiling.keys, [&](const domain::TilingKey& key) { return key.command == command; });
        if (const std::string keys = text(value); !keys.empty())
        {
            auto parsed = domain::TilingKey::parse(command, keys);
            if (!parsed)
            {
                showProblem(parsed.error().message());
                return;
            }
            next.tiling.keys.push_back(*parsed);
            std::ranges::sort(next.tiling.keys, {}, &domain::TilingKey::command);
        }
        if (const auto problem = domain::tilingProblem(next))
        {
            showProblem(*problem);
            return;
        }
        saveTiling(std::move(next));
    });
    api.on_tiling_keys_default([this] {
        if (m_settings)
        {
            domain::Settings next = *m_settings;
            next.tiling.keys = domain::defaultTilingKeys();
            saveTiling(std::move(next));
        }
    });
    api.on_tiling_rule_add([this] {
        if (m_settings)
        {
            domain::Settings next = *m_settings;
            next.tiling.rules.push_back({"Calculator.exe", {}, {}, domain::TilingRuleAction::Float, 1});
            saveTiling(std::move(next));
        }
    });
    api.on_tiling_rule_edited([this](int index, const TilingRuleRow& row) {
        if (!m_settings || index < 0 || static_cast<std::size_t>(index) >= m_settings->tiling.rules.size())
        {
            return;
        }
        domain::Settings next = *m_settings;
        domain::TilingRule& rule = next.tiling.rules[static_cast<std::size_t>(index)];
        rule.app = text(row.app);
        rule.windowClass = text(row.window_class);
        rule.title = text(row.title);
        rule.action = static_cast<domain::TilingRuleAction>(std::clamp(row.action, 0, 2));
        rule.workspace = std::clamp(row.workspace, 1, domain::kMaxWorkspaces);
        if (!saveTiling(std::move(next)))
        {
            refreshSettings();
        }
    });
    api.on_tiling_rule_remove([this](int index) {
        if (m_settings && index >= 0 && static_cast<std::size_t>(index) < m_settings->tiling.rules.size())
        {
            domain::Settings next = *m_settings;
            next.tiling.rules.erase(next.tiling.rules.begin() + index);
            saveTiling(std::move(next));
        }
    });
}

} // namespace rmw::settingsapp
