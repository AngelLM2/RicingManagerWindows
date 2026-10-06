#include "StartMenu.hpp"

#include "Widgets.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/providers/Windows.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <powrprof.h>
#define SECURITY_WIN32
#include <security.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <format>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

constexpr int kMenuWidth = 640;   // logical pixels
constexpr int kMenuHeight = 700;
constexpr int kMenuGap = 8;       // from the bar
constexpr int kIconSize = 48;
constexpr std::size_t kResults = 24;
constexpr std::size_t kIconsPerTick = 12;
constexpr auto kIconTick = std::chrono::milliseconds(30);
constexpr auto kIconBudget = std::chrono::milliseconds(8);
constexpr auto kReopenGuard = std::chrono::milliseconds(300);

bool g_wantsWindow = false; // the platform's next window is the menu's
MenuWindow* g_created = nullptr;

constexpr std::string_view kSource = R"slint(
import { LineEdit, ListView, ScrollView } from "std-widgets.slint";

export struct RmwStartWords {
    search: string,
    search-label: string,
    no-match: string,
    all-apps: string,
    pinned: string,
    back: string,
    all-apps-button: string,
    nothing-pinned: string,
    folder: string,
    documents: string,
    downloads: string,
    pictures: string,
    settings: string,
    power: string,
    lock: string,
    sleep: string,
    sign-out: string,
    restart: string,
    shut-down: string,
}

export struct RmwStartApp {
    id: string,
    name: string,
    icon: image,
}

component RmwStartGlyphButton inherits Rectangle {
    in property <string> glyph;
    in property <string> label;
    in property <color> foreground;
    callback clicked();
    width: 40px;
    height: 40px;
    border-radius: 8px;
    background: touch.pressed ? root.foreground.transparentize(82%) : touch.has-hover ? root.foreground.transparentize(90%) : transparent;
    animate background { duration: 120ms; easing: ease-out; }
    accessible-role: button;
    accessible-label: root.label;
    accessible-action-default => { root.clicked(); }
    Text {
        text: root.glyph;
        font-family: "Segoe Fluent Icons";
        font-size: 16px;
        color: root.foreground;
        horizontal-alignment: center;
        vertical-alignment: center;
    }
    touch := TouchArea {
        clicked => { root.clicked(); }
    }
}

component RmwStartTile inherits Rectangle {
    in property <RmwStartApp> app;
    in property <color> foreground;
    callback launch(string);
    width: 96px;
    height: 88px;
    border-radius: 8px;
    background: touch.pressed ? root.foreground.transparentize(84%) : touch.has-hover ? root.foreground.transparentize(91%) : transparent;
    animate background { duration: 120ms; easing: ease-out; }
    accessible-role: button;
    accessible-label: root.app.name;
    accessible-action-default => { root.launch(root.app.id); }
    VerticalLayout {
        padding: 8px;
        spacing: 6px;
        alignment: center;
        HorizontalLayout {
            alignment: center;
            Image {
                source: root.app.icon;
                width: 32px;
                height: 32px;
                image-fit: contain;
                image-rendering: smooth;
            }
        }
        Text {
            text: root.app.name;
            color: root.foreground;
            font-size: 11px;
            horizontal-alignment: center;
            overflow: elide;
        }
    }
    touch := TouchArea {
        clicked => { root.launch(root.app.id); }
    }
}

component RmwStartRow inherits Rectangle {
    in property <RmwStartApp> app;
    in property <color> foreground;
    in property <color> accent;
    in property <bool> first;
    callback launch(string);
    height: 40px;
    border-radius: 6px;
    background: touch.pressed ? root.foreground.transparentize(84%)
        : touch.has-hover ? root.foreground.transparentize(91%)
        : root.first ? root.accent.transparentize(80%) : transparent;
    accessible-role: button;
    accessible-label: root.app.name;
    accessible-action-default => { root.launch(root.app.id); }
    HorizontalLayout {
        padding-left: 10px;
        padding-right: 10px;
        spacing: 12px;
        VerticalLayout {
            alignment: center;
            Image {
                source: root.app.icon;
                width: 24px;
                height: 24px;
                image-fit: contain;
                image-rendering: smooth;
            }
        }
        Text {
            text: root.app.name;
            color: root.foreground;
            font-size: 13px;
            vertical-alignment: center;
            overflow: elide;
        }
    }
    touch := TouchArea {
        clicked => { root.launch(root.app.id); }
    }
}

export component RmwStartMenu inherits Window {
    in property <brush> menu-matte: #000000;
    background: root.menu-matte;
    in property <color> tint: #202020f0;
    in property <color> foreground: #ffffff;
    in property <color> accent: #60cdff;
    in property <length> radius: 12px;
    in property <[RmwStartApp]> pinned;
    in property <[RmwStartApp]> apps;
    in property <[RmwStartApp]> results;
    in property <string> user;
    in property <RmwStartWords> words;
    in-out property <string> query;
    in-out property <bool> all-apps;
    in-out property <bool> power-open;
    in-out property <bool> open;
    callback search(string);
    callback launch(string);
    callback power(string);
    callback place(string);
    callback dismiss();
    out property <bool> searching: root.query != "";

    forward-focus: field;

    // Esc closes; Enter opens the first match (LineEdit's accepted).
    FocusScope {
        key-pressed(event) => {
            if (event.text == Key.Escape) {
                if (root.power-open) {
                    root.power-open = false;
                } else {
                    root.dismiss();
                }
                return accept;
            }
            reject
        }

        panel := Rectangle {
            y: root.open ? 0px : 18px;
            opacity: root.open ? 1 : 0;
            animate y, opacity { duration: 170ms; easing: ease-out; }
            border-radius: root.radius;
            background: root.tint;
            border-width: 1px;
            border-color: root.foreground.transparentize(86%);
            clip: true;

            VerticalLayout {
                padding: 22px;
                padding-bottom: 12px;
                spacing: 16px;

                field := LineEdit {
                    placeholder-text: root.words.search;
                    text <=> root.query;
                    accessible-label: root.words.search-label;
                    edited(text) => { root.search(text); }
                    accepted(text) => {
                        if (root.results.length > 0) {
                            root.launch(root.results[0].id);
                        }
                    }
                }

                // While typing: what matches.
                if root.searching: ScrollView {
                    vertical-stretch: 1;
                    VerticalLayout {
                        spacing: 2px;
                        for app[i] in root.results: RmwStartRow {
                            app: app;
                            first: i == 0;
                            foreground: root.foreground;
                            accent: root.accent;
                            launch(id) => { root.launch(id); }
                        }
                        if root.results.length == 0: Text {
                            text: root.words.no-match;
                            color: root.foreground.transparentize(40%);
                            font-size: 13px;
                        }
                    }
                }

                // Pinned, or every app.
                if !root.searching: HorizontalLayout {
                    Text {
                        text: root.all-apps ? root.words.all-apps : root.words.pinned;
                        color: root.foreground;
                        font-size: 14px;
                        font-weight: 600;
                        vertical-alignment: center;
                    }
                    Rectangle { horizontal-stretch: 1; }
                    Rectangle {
                        width: switch-text.preferred-width + 24px;
                        height: 28px;
                        border-radius: 6px;
                        background: switch-touch.has-hover ? root.foreground.transparentize(88%) : root.foreground.transparentize(94%);
                        accessible-role: button;
                        accessible-label: switch-text.text;
                        accessible-action-default => { root.all-apps = !root.all-apps; }
                        switch-text := Text {
                            text: root.all-apps ? root.words.back : root.words.all-apps-button;
                            color: root.foreground;
                            font-size: 12px;
                            horizontal-alignment: center;
                            vertical-alignment: center;
                        }
                        switch-touch := TouchArea {
                            clicked => { root.all-apps = !root.all-apps; }
                        }
                    }
                }

                if !root.searching && !root.all-apps: Rectangle {
                    vertical-stretch: 1;
                    for app[i] in root.pinned: RmwStartTile {
                        x: Math.mod(i, 6) * 98px;
                        y: Math.floor(i / 6) * 92px;
                        app: app;
                        foreground: root.foreground;
                        launch(id) => { root.launch(id); }
                    }
                    if root.pinned.length == 0: Text {
                        y: 0px;
                        text: root.words.nothing-pinned;
                        color: root.foreground.transparentize(40%);
                        font-size: 12px;
                        wrap: word-wrap;
                    }
                }

                // Only the rows in sight exist: a PC with hundreds of apps costs what a screenful does.
                if !root.searching && root.all-apps: ListView {
                    vertical-stretch: 1;
                    for app in root.apps: RmwStartRow {
                        app: app;
                        foreground: root.foreground;
                        accent: root.accent;
                        launch(id) => { root.launch(id); }
                    }
                }

                // The user, their places, Settings and power.
                Rectangle {
                    height: 1px;
                    background: root.foreground.transparentize(88%);
                }
                HorizontalLayout {
                    spacing: 4px;
                    height: 44px;
                    RmwStartGlyphButton {
                        glyph: "\u{E77B}";
                        label: root.words.folder;
                        foreground: root.foreground;
                        clicked => { root.place("home"); }
                    }
                    Text {
                        text: root.user;
                        color: root.foreground;
                        font-size: 13px;
                        vertical-alignment: center;
                        overflow: elide;
                    }
                    Rectangle { horizontal-stretch: 1; }
                    RmwStartGlyphButton {
                        glyph: "\u{E8B7}";
                        label: root.words.documents;
                        foreground: root.foreground;
                        clicked => { root.place("documents"); }
                    }
                    RmwStartGlyphButton {
                        glyph: "\u{E896}";
                        label: root.words.downloads;
                        foreground: root.foreground;
                        clicked => { root.place("downloads"); }
                    }
                    RmwStartGlyphButton {
                        glyph: "\u{E91B}";
                        label: root.words.pictures;
                        foreground: root.foreground;
                        clicked => { root.place("pictures"); }
                    }
                    RmwStartGlyphButton {
                        glyph: "\u{E713}";
                        label: root.words.settings;
                        foreground: root.foreground;
                        clicked => { root.place("settings"); }
                    }
                    RmwStartGlyphButton {
                        glyph: "\u{E7E8}";
                        label: root.words.power;
                        foreground: root.foreground;
                        clicked => { root.power-open = !root.power-open; }
                    }
                }
            }

            // The power choices, over the corner they came from.
            if root.power-open: Rectangle {
                x: parent.width - self.width - 16px;
                y: parent.height - self.height - 60px;
                width: 200px;
                height: power-list.preferred-height;
                border-radius: 8px;
                background: root.tint.with-alpha(1);
                border-width: 1px;
                border-color: root.foreground.transparentize(80%);
                power-list := VerticalLayout {
                    padding: 6px;
                    for choice in [
                        { id: "lock", glyph: "\u{E72E}", label: root.words.lock },
                        { id: "sleep", glyph: "\u{E708}", label: root.words.sleep },
                        { id: "signout", glyph: "\u{F3B1}", label: root.words.sign-out },
                        { id: "restart", glyph: "\u{E777}", label: root.words.restart },
                        { id: "shutdown", glyph: "\u{E7E8}", label: root.words.shut-down },
                    ]: Rectangle {
                        height: 36px;
                        border-radius: 6px;
                        background: choice-touch.has-hover ? root.foreground.transparentize(90%) : transparent;
                        accessible-role: button;
                        accessible-label: choice.label;
                        accessible-action-default => { root.power(choice.id); }
                        HorizontalLayout {
                            padding-left: 10px;
                            spacing: 12px;
                            Text {
                                text: choice.glyph;
                                font-family: "Segoe Fluent Icons";
                                font-size: 14px;
                                color: root.foreground;
                                vertical-alignment: center;
                            }
                            Text {
                                text: choice.label;
                                color: root.foreground;
                                font-size: 13px;
                                vertical-alignment: center;
                            }
                        }
                        choice-touch := TouchArea {
                            clicked => { root.power(choice.id); }
                        }
                    }
                }
            }
        }
    }
}
)slint";

struct Words
{
    std::string_view search, searchLabel, noMatch, allApps, pinned, back, allAppsButton, nothingPinned, folder,
        documents, downloads, pictures, settings, power, lock, sleep, signOut, restart, shutDown;
};

constexpr Words kEnglish{"Type to search apps", "Search apps", "No app is called that.", "All apps", "Pinned",
                         "\u2039 Back", "All apps \u203A",
                         "Nothing pinned yet: the rice's [start] pinned, or Settings > Taskbar.", "Your folder",
                         "Documents", "Downloads", "Pictures", "Settings", "Power", "Lock", "Sleep", "Sign out",
                         "Restart", "Shut down"};
constexpr Words kPortuguese{"Digite para procurar apps", "Procurar apps", "Nenhum app com esse nome.",
                            "Todos os apps", "Fixados", "\u2039 Voltar", "Todos os apps \u203A",
                            "Nada fixado ainda: o [start] pinned do rice, ou Configura\u00E7\u00F5es > Barra de tarefas.",
                            "Sua pasta", "Documentos", "Downloads", "Imagens", "Configura\u00E7\u00F5es", "Energia",
                            "Bloquear", "Suspender", "Sair", "Reiniciar", "Desligar"};

Value wordsValue(const Words& w)
{
    const auto text = [](std::string_view value) { return Value(slint::SharedString(value)); };
    return Value(slint::interpreter::Struct{
        {"search", text(w.search)},           {"search-label", text(w.searchLabel)},
        {"no-match", text(w.noMatch)},         {"all-apps", text(w.allApps)},
        {"pinned", text(w.pinned)},            {"back", text(w.back)},
        {"all-apps-button", text(w.allAppsButton)}, {"nothing-pinned", text(w.nothingPinned)},
        {"folder", text(w.folder)},            {"documents", text(w.documents)},
        {"downloads", text(w.downloads)},      {"pictures", text(w.pictures)},
        {"settings", text(w.settings)},        {"power", text(w.power)},
        {"lock", text(w.lock)},                {"sleep", text(w.sleep)},
        {"sign-out", text(w.signOut)},         {"restart", text(w.restart)},
        {"shut-down", text(w.shutDown)}});
}

// Lower case, without accents: what search compares (á = a, Ç = c).
std::wstring folded(std::wstring_view text)
{
    std::wstring out(text);
    if (!out.empty())
    {
        std::array<wchar_t, 512> buffer{};
        const int written = FoldStringW(MAP_COMPOSITE, out.c_str(), static_cast<int>(out.size()), buffer.data(),
                                        static_cast<int>(buffer.size()));
        if (written > 0)
        {
            out.clear();
            for (int i = 0; i < written; ++i)
            {
                const wchar_t c = buffer[static_cast<std::size_t>(i)];
                if (c < 0x0300 || c > 0x036F) // the accents, now apart: dropped
                {
                    out += c;
                }
            }
        }
    }
    std::ranges::transform(out, out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

// A user's display name ("Ana Souza"), else their account name.
std::string userName()
{
    std::array<wchar_t, 256> name{};
    auto size = static_cast<ULONG>(name.size());
    if (GetUserNameExW(NameDisplay, name.data(), &size) != FALSE && name[0] != L'\0')
    {
        return utf16To8Lossy(name.data());
    }
    auto length = static_cast<DWORD>(name.size());
    return GetUserNameW(name.data(), &length) != FALSE ? utf16To8Lossy(name.data()) : std::string();
}

void openKnownFolder(const KNOWNFOLDERID& folder)
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path)))
    {
        ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    }
    CoTaskMemFree(path);
}

// Shutting down, restarting and signing out need the shutdown privilege, which a user has but
// which is off in a process until asked for.
bool enableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE)
    {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) != FALSE &&
                    AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) != FALSE &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

Value appValue(const platform::InstalledApp& app, const slint::Image& icon)
{
    return Value(slint::interpreter::Struct{{"id", Value(slint::SharedString(app.id))},
                                            {"name", Value(slint::SharedString(app.name))},
                                            {"icon", Value(icon)}});
}

} // namespace

bar::Rect startMenuRect(const StartAnchor& anchor, std::int32_t width, std::int32_t height) noexcept
{
    const auto gap = static_cast<std::int32_t>(std::lround(kMenuGap * anchor.scale));
    const bar::Rect& work = anchor.work;
    width = std::min(width, work.width() - (2 * gap));
    height = std::min(height, work.height() - (2 * gap));
    bar::Rect menu;
    const auto clampX = [&](std::int32_t left) { return std::clamp(left, work.left + gap, work.right - gap - width); };
    const auto clampY = [&](std::int32_t top) { return std::clamp(top, work.top + gap, work.bottom - gap - height); };
    switch (anchor.edge)
    {
    case domain::BarPosition::Bottom:
    case domain::BarPosition::Top: {
        const std::int32_t left = anchor.centered ? anchor.bar.left + ((anchor.bar.width() - width) / 2)
                                                  : anchor.bar.left + gap;
        const std::int32_t top = anchor.edge == domain::BarPosition::Bottom ? anchor.bar.top - gap - height
                                                                            : anchor.bar.bottom + gap;
        menu.left = clampX(left);
        menu.top = clampY(top);
        break;
    }
    case domain::BarPosition::Left:
    case domain::BarPosition::Right: {
        const std::int32_t left = anchor.edge == domain::BarPosition::Left ? anchor.bar.right + gap
                                                                           : anchor.bar.left - gap - width;
        const std::int32_t top = anchor.centered ? anchor.bar.top + ((anchor.bar.height() - height) / 2)
                                                 : anchor.bar.top + gap;
        menu.left = clampX(left);
        menu.top = clampY(top);
        break;
    }
    }
    menu.right = menu.left + width;
    menu.bottom = menu.top + height;
    return menu;
}

std::vector<std::size_t> searchApps(std::span<const platform::InstalledApp> apps, std::string_view query,
                                    std::size_t limit)
{
    const std::wstring wanted = folded(utf8To16Lossy(query));
    std::vector<std::pair<int, std::size_t>> ranked; // rank (lower is better), index
    if (wanted.empty())
    {
        return {};
    }
    for (std::size_t i = 0; i < apps.size(); ++i)
    {
        const std::wstring name = folded(utf8To16Lossy(apps[i].name));
        const auto at = name.find(wanted);
        if (at == std::wstring::npos)
        {
            continue;
        }
        const bool wordStart = at == 0 || std::iswalnum(name[at - 1]) == 0;
        ranked.emplace_back(at == 0 ? 0 : wordStart ? 1 : 2, i);
    }
    std::ranges::stable_sort(ranked, {}, &std::pair<int, std::size_t>::first);
    std::vector<std::size_t> out;
    for (const auto& [rank, index] : ranked)
    {
        if (out.size() >= limit)
        {
            break;
        }
        out.push_back(index);
    }
    return out;
}

std::string_view startMenuSource() noexcept
{
    return kSource;
}

bool startMenuWantsWindow() noexcept
{
    return g_wantsWindow;
}

std::unique_ptr<slint::platform::WindowAdapter> makeStartMenuWindow()
{
    auto window = std::make_unique<MenuWindow>();
    g_created = window.get();
    return window;
}

std::unique_ptr<StartMenu> StartMenu::create(const bar::StartPlan& plan)
{
    slint::interpreter::ComponentCompiler compiler;
    auto definition = compiler.build_from_source(std::string(kSource), "start.slint");
    if (!definition)
    {
        for (const auto& diagnostic : compiler.diagnostics())
        {
            log::error("start menu: start.slint:{}: {}", diagnostic.line, std::string(diagnostic.message));
        }
        return nullptr;
    }
    std::unique_ptr<StartMenu> menu(new StartMenu());
    menu->m_plan = plan;
    g_wantsWindow = true;
    g_created = nullptr;
    menu->m_instance = definition->create();
    g_wantsWindow = false;
    menu->m_window = g_created;
    g_created = nullptr;
    if (menu->m_window == nullptr)
    {
        log::error("start menu: no window");
        return nullptr;
    }
    auto& instance = *menu->m_instance;
    StartMenu* self = menu.get();
    const slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> weak(instance);
    menu->m_window->setMatte([weak](bool white) {
        if (auto strong = weak.lock())
        {
            const auto matte = white ? slint::Color::from_rgb_uint8(255, 255, 255) : slint::Color::from_rgb_uint8(0, 0, 0);
            (*strong)->set_property("menu-matte", Value(slint::Brush(matte)));
        }
    });
    menu->m_window->setOnHidden([self] {
        if (auto& i = self->m_instance)
        {
            (*i)->set_property("open", Value(false));
            (*i)->set_property("query", Value(slint::SharedString()));
            (*i)->set_property("all-apps", Value(false));
            (*i)->set_property("power-open", Value(false));
        }
        self->m_results.clear();
    });
    instance->set_callback("search", [self](std::span<const Value> args) {
        if (!args.empty())
        {
            if (auto text = args[0].to_string())
            {
                self->search(std::string_view(*text));
            }
        }
        return Value();
    });
    instance->set_callback("launch", [self](std::span<const Value> args) {
        if (!args.empty())
        {
            if (auto id = args[0].to_string())
            {
                self->launch(std::string_view(*id));
            }
        }
        return Value();
    });
    instance->set_callback("power", [self](std::span<const Value> args) {
        if (!args.empty())
        {
            if (auto action = args[0].to_string())
            {
                self->power(std::string_view(*action));
            }
        }
        return Value();
    });
    instance->set_callback("place", [self](std::span<const Value> args) {
        if (!args.empty())
        {
            if (auto name = args[0].to_string())
            {
                self->place(std::string_view(*name));
            }
        }
        return Value();
    });
    instance->set_callback("dismiss", [self](std::span<const Value>) {
        self->hide();
        return Value();
    });
    instance->set_property("user", Value(slint::SharedString(userName())));
    instance->set_property("words", wordsValue(plan.language == "pt-BR" ? kPortuguese : kEnglish));
    instance->show(); // prepared, not shown: MenuWindow shows it on toggle()
    menu->load();
    // Its first frame, unseen, at the main monitor's scale.
    UINT dpiX = 96;
    UINT dpiY = 96;
    GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const double scale = dpiX / 96.0;
    menu->m_window->warmUp({0, 0, static_cast<std::int32_t>(std::lround(kMenuWidth * scale)),
                            static_cast<std::int32_t>(std::lround(kMenuHeight * scale))},
                           scale);
    return menu;
}

StartMenu::~StartMenu()
{
    m_iconTimer.stop();
    if (m_instance)
    {
        (*m_instance)->hide();
    }
}

void StartMenu::setLook(slint::Color background, slint::Color foreground, slint::Color accent, double radius)
{
    if (!m_instance)
    {
        return;
    }
    auto& instance = *m_instance;
    instance->set_property("tint", Value(slint::Brush(background)));
    instance->set_property("foreground", Value(slint::Brush(foreground)));
    instance->set_property("accent", Value(slint::Brush(accent)));
    instance->set_property("radius", Value(std::clamp(radius, 0.0, 24.0)));
}

bool StartMenu::shown() const noexcept
{
    return m_window != nullptr && m_window->shown();
}

void StartMenu::toggle(const StartAnchor& anchor)
{
    if (m_window == nullptr || !m_instance)
    {
        return;
    }
    if (m_window->shown())
    {
        hide();
        return;
    }
    if (m_window->sinceHidden() < kReopenGuard)
    {
        return; // the click that closed it
    }
    const auto width = static_cast<std::int32_t>(std::lround(kMenuWidth * anchor.scale));
    const auto height = static_cast<std::int32_t>(std::lround(kMenuHeight * anchor.scale));
    const bar::Rect rect = startMenuRect(anchor, width, height);
    m_window->showAt(rect, anchor.scale);
    (*m_instance)->set_property("open", Value(true));
    if (m_nextIcon < m_apps.size())
    {
        m_iconTimer.start(slint::TimerMode::Repeated, kIconTick, [this] { loadIcons(); });
    }
}

void StartMenu::hide()
{
    if (m_window != nullptr)
    {
        m_window->hide();
    }
    m_iconTimer.stop();
}

void StartMenu::load()
{
    auto apps = platform::installedApps();
    if (!apps)
    {
        log::warn("start menu: the app list: {}", apps.error().toString());
        return;
    }
    m_apps = std::move(*apps);
    m_icons.assign(m_apps.size(), slint::Image());
    m_iconLoaded.assign(m_apps.size(), false);
    // Pinned first: their icons now, the rest a few at a time while the menu is open.
    for (const auto& id : m_plan.pinned)
    {
        const auto found = std::ranges::find(m_apps, id, &platform::InstalledApp::id);
        if (found != m_apps.end())
        {
            m_pinnedApps.push_back(static_cast<std::size_t>(found - m_apps.begin()));
        }
    }
    m_pinnedModel = std::make_shared<slint::VectorModel<Value>>();
    for (const std::size_t index : m_pinnedApps)
    {
        m_icons[index] = imageFromPixels(providers::appIconPixels(utf8To16Lossy(m_apps[index].id), kIconSize));
        m_iconLoaded[index] = true;
        m_pinnedModel->push_back(appValue(m_apps[index], m_icons[index]));
    }
    m_allModel = std::make_shared<slint::VectorModel<Value>>();
    for (std::size_t i = 0; i < m_apps.size(); ++i)
    {
        m_allModel->push_back(appValue(m_apps[i], m_icons[i]));
    }
    (*m_instance)->set_property("pinned", Value(std::shared_ptr<slint::Model<Value>>(m_pinnedModel)));
    (*m_instance)->set_property("apps", Value(std::shared_ptr<slint::Model<Value>>(m_allModel)));
    log::info("start menu: {} apps, {} pinned", m_apps.size(), m_pinnedApps.size());
}

void StartMenu::loadIcons()
{
    // A few at a time and never long: the Shell takes from 1 to 50 ms per icon, and the menu scrolls
    // and types meanwhile.
    const auto until = std::chrono::steady_clock::now() + kIconBudget;
    std::size_t loaded = 0;
    while (m_nextIcon < m_apps.size() && loaded < kIconsPerTick && std::chrono::steady_clock::now() < until)
    {
        const std::size_t i = m_nextIcon++;
        if (m_iconLoaded[i])
        {
            continue;
        }
        m_icons[i] = imageFromPixels(providers::appIconPixels(utf8To16Lossy(m_apps[i].id), kIconSize));
        m_iconLoaded[i] = true;
        m_allModel->set_row_data(i, appValue(m_apps[i], m_icons[i]));
        ++loaded;
    }
    if (m_nextIcon >= m_apps.size())
    {
        m_iconTimer.stop();
    }
}

void StartMenu::search(std::string_view query)
{
    m_results = searchApps(m_apps, query, kResults);
    auto model = std::make_shared<slint::VectorModel<Value>>();
    for (const std::size_t index : m_results)
    {
        if (!m_iconLoaded[index])
        {
            m_icons[index] = imageFromPixels(providers::appIconPixels(utf8To16Lossy(m_apps[index].id), kIconSize));
            m_iconLoaded[index] = true;
        }
        model->push_back(appValue(m_apps[index], m_icons[index]));
    }
    (*m_instance)->set_property("results", Value(std::shared_ptr<slint::Model<Value>>(model)));
}

void StartMenu::launch(std::string_view id)
{
    hide();
    platform::launchAppInBackground(std::string(id), "start menu");
}

void StartMenu::place(std::string_view name)
{
    hide();
    if (name == "home")
    {
        openKnownFolder(FOLDERID_Profile);
    }
    else if (name == "documents")
    {
        openKnownFolder(FOLDERID_Documents);
    }
    else if (name == "downloads")
    {
        openKnownFolder(FOLDERID_Downloads);
    }
    else if (name == "pictures")
    {
        openKnownFolder(FOLDERID_Pictures);
    }
    else if (name == "settings")
    {
        ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void StartMenu::power(std::string_view action)
{
    hide();
    log::info("start menu: {}", action);
    if (action == "lock")
    {
        LockWorkStation();
        return;
    }
    if (action == "sleep")
    {
        SetSuspendState(FALSE, FALSE, FALSE);
        return;
    }
    // EWX_LOGOFF is 0: what to do is told apart by name, not by flags.
    const bool signOut = action == "signout";
    const UINT how = action == "restart" ? EWX_REBOOT : action == "shutdown" ? EWX_SHUTDOWN | EWX_POWEROFF : EWX_LOGOFF;
    if (!signOut && action != "restart" && action != "shutdown")
    {
        return;
    }
    if (!signOut && !enableShutdownPrivilege())
    {
        log::warn("start menu: no shutdown privilege");
        return;
    }
    // Apps are asked to close, as when Windows' own Start does it.
    ExitWindowsEx(how, SHTDN_REASON_FLAG_PLANNED); // reason: other (0), planned
}

} // namespace rmw::barapp
