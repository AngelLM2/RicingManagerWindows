#pragma once

#include "MenuWindow.hpp"

#include "rmw/bar/Geometry.hpp"
#include "rmw/bar/Plan.hpp"
#include "rmw/platform/Apps.hpp"
#include "rmw/providers/Windows.hpp"

#include <slint-interpreter.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmw::barapp
{

// Where the menu opens from: the bar whose Start button was clicked (or the main one, for the Win key).
struct StartAnchor
{
    bar::Rect bar;          // the bar window, physical pixels
    bar::Rect work;         // its monitor's work area
    double scale = 1.0;
    domain::BarPosition edge = domain::BarPosition::Bottom;
    bool centered = false;  // the Start button is in the bar's middle (Windows 11's place)
};

// Where a menu of `width` x `height` physical pixels goes: next to the bar, on the Start button's
// side (or in the middle), inside the work area.
[[nodiscard]] bar::Rect startMenuRect(const StartAnchor& anchor, std::int32_t width, std::int32_t height) noexcept;

// The apps that match what is typed, best first: the name starts with it, then a word of it does,
// then it is somewhere in it (letters compared without case or accents); at most `limit`.
[[nodiscard]] std::vector<std::size_t> searchApps(std::span<const platform::InstalledApp> apps,
                                                  std::string_view query, std::size_t limit);

// Ricing Manager's Start menu (M11): all apps and a search, the rice's pinned apps, the user's
// folders, Settings and the power buttons, in the rice's colors. Lives in rmw-bar, opened by its
// Start button and, when the user wants, by the Win key.
class StartMenu
{
public:
    // Compiles the menu; `create` makes its window (the bar's platform asks BarApp for one).
    static std::unique_ptr<StartMenu> create(const bar::StartPlan& plan);
    ~StartMenu();
    StartMenu(const StartMenu&) = delete;
    StartMenu& operator=(const StartMenu&) = delete;
    StartMenu(StartMenu&&) = delete;
    StartMenu& operator=(StartMenu&&) = delete;

    // The rice's look: its tint, text, accent and corners.
    void setLook(slint::Color background, slint::Color foreground, slint::Color accent, double radius);
    // Opens it next to `anchor`, or closes it when open (or when it just closed by itself: the
    // click on the button that made it go is not another open).
    void toggle(const StartAnchor& anchor);
    [[nodiscard]] bool shown() const noexcept;

    using Instance = slint::ComponentHandle<slint::interpreter::ComponentInstance>;

private:
    using Value = slint::interpreter::Value;
    using Batch = std::vector<std::pair<std::size_t, providers::IconPixels>>;
    // Where the loader hands its work back: the menu, or null once it is gone.
    using Self = std::shared_ptr<std::atomic<StartMenu*>>;

    StartMenu() = default;
    // The app list and every icon, pinned ones first, on a thread of its own (the Shell takes up to
    // 50 ms per icon and hundreds of milliseconds for the list: on the bar's thread a rebuild or a typed
    // letter could take it past its heartbeat). What it finds comes back to onApps() and onIcons().
    static void loadInBackground(Self self, std::vector<std::string> pinned);
    void onApps(std::vector<platform::InstalledApp> apps);
    void onIcons(Batch icons);
    void search(std::string_view query);
    void launch(std::string_view id);
    void power(std::string_view action);
    void place(std::string_view name);
    void hide();

    std::optional<Instance> m_instance;
    MenuWindow* m_window = nullptr; // Slint owns it
    bar::StartPlan m_plan;
    Self m_self = std::make_shared<std::atomic<StartMenu*>>(nullptr);
    std::vector<platform::InstalledApp> m_apps;
    std::vector<slint::Image> m_icons;  // by app index; empty until loaded
    std::shared_ptr<slint::VectorModel<Value>> m_allModel;
    std::shared_ptr<slint::VectorModel<Value>> m_pinnedModel;
    std::shared_ptr<slint::VectorModel<Value>> m_resultsModel;
    std::vector<std::size_t> m_pinnedApps; // indexes into m_apps
    std::vector<std::size_t> m_results;
};

// The menu's Slint (the tests compile and draw it without a window).
[[nodiscard]] std::string_view startMenuSource() noexcept;

// The window the platform hands the menu (BarApp asks for it while the menu is being made).
[[nodiscard]] bool startMenuWantsWindow() noexcept;
[[nodiscard]] std::unique_ptr<slint::platform::WindowAdapter> makeStartMenuWindow();

} // namespace rmw::barapp
