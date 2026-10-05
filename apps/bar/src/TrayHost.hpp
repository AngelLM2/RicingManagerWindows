#pragma once

#include "rmw/bar/Tray.hpp"
#include "rmw/providers/Windows.hpp"

#include <slint.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rmw::barapp
{

// The system tray in our bar (docs/04 M5, ADR-0009, ADR-0012). A window of class "Shell_TrayWnd" in
// front of Explorer's, so Shell_NotifyIcon reaches it first. Every call also goes on to Explorer's
// tray, so apps keep their icons there and have them whenever we go away. We keep a copy (the
// pixels too: the app may destroy its HICON) and send clicks back to the apps. No code goes into
// another process.
//
// The window lives on a thread of its own: apps wait for it inside Shell_NotifyIcon, and the bar's
// UI must never make them wait. It answers them at once; TrayForwarder keeps Explorer's tray in
// step on another thread, so app bars (ours too: SHAppBarMessage reaches this window) never wait
// behind every app's icons while Explorer is slow. Started by the first user (a tray widget, a
// plugin watching "tray"); stopped a few seconds after the last one goes, so a rebuild of the bars
// keeps it. One per process; everything but the tray thread itself runs on the UI thread.
class TrayHost
{
public:
    struct IconView
    {
        bar::tray::Icon icon;
        std::string tip; // UTF-8
        std::string app; // the executable's file name: "Discord.exe"
        std::uint32_t pid = 0;
        bool promoted = true;       // shown in the bar by Windows' own setting ("Other system tray icons")
        std::uint64_t revision = 0; // changes when the pixels do
        providers::IconPixels pixels;
    };

    static TrayHost& instance();
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;
    TrayHost(TrayHost&&) = delete;
    TrayHost& operator=(TrayHost&&) = delete;

    [[nodiscard]] std::size_t subscribe(std::function<void()> changed);
    void unsubscribe(std::size_t id);
    // Before the process ends: the window goes now (apps keep their icons in Explorer's tray).
    void shutdown();

    // In the order apps added them (hidden ones included: NIS_HIDDEN).
    [[nodiscard]] const std::vector<IconView>& icons() const noexcept { return m_icons; }
    [[nodiscard]] const IconView* find(std::uint64_t number) const noexcept;

    // What the user did on an icon, at screen point (x, y). The bar got that input, so it may let
    // the app come to the front (its menus and windows need it).
    void gesture(std::uint64_t number, bar::tray::Gesture gesture, int x, int y);

    // What the tray thread and the UI thread both see.
    struct Shared;
    // From the tray thread (through the event loop): the icons now. Ignored when `from` is a tray
    // window that is gone already.
    void receive(const std::shared_ptr<Shared>& from, std::vector<IconView> icons);

private:
    TrayHost();
    ~TrayHost();
    void start();
    void stop();

    std::shared_ptr<Shared> m_shared;
    std::jthread m_thread;
    std::vector<IconView> m_icons;
    std::map<std::size_t, std::function<void()>> m_subscribers;
    std::size_t m_nextSubscriber = 1;
    std::unique_ptr<slint::Timer> m_stopTimer; // gone at shutdown, before Slint is
};

} // namespace rmw::barapp
