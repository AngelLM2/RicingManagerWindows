#pragma once

#include "rmw/bar/Source.hpp"
#include "rmw/providers/Windows.hpp"

#include <slint-interpreter.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rmw::barapp
{

using Instance = slint::ComponentHandle<slint::interpreter::ComponentInstance>;

// The widgets built into the bar (docs/04 M3). Each is a .slint component plus a provider that
// feeds it; plugins (M4) bring their own components and their data comes from the plugin host.
[[nodiscard]] std::span<const bar::WidgetKind> builtinKinds() noexcept;
[[nodiscard]] std::string_view builtinSources() noexcept;

// Feeds one slot of a bar: sets its properties, answers its callbacks, keeps its timers.
class Provider
{
public:
    virtual ~Provider() = default;
    Provider() = default;
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    Provider(Provider&&) = delete;
    Provider& operator=(Provider&&) = delete;
};

// What the Start button does first (M11): opens Ricing Manager's Start when the rice uses it (true),
// else Windows' opens. Set by BarApp; empty: Windows'.
void setStartOpener(std::function<bool()> opener);

// nullptr for a widget that needs nothing.
[[nodiscard]] std::unique_ptr<Provider> makeProvider(const bar::Slot& slot, const Instance& instance);

// A widget's settings from the rice (`{ id = "clock", format = "%H:%M" }`), with a fallback when
// absent or of another type.
[[nodiscard]] std::string settingString(const domain::BarItem& item, std::string_view key,
                                        std::string_view fallback);
[[nodiscard]] bool settingBool(const domain::BarItem& item, std::string_view key, bool fallback);

// Icon pixels (rmw::providers::iconPixels) as a Slint image; empty pixels give an empty image.
[[nodiscard]] slint::Image imageFromPixels(const providers::IconPixels& pixels);

// "%H:%M" with a safe, small set of conversions (a rice is untrusted: no strftime, whose invalid
// conversions end the process): %H %I %M %S %p %d %m %y %Y, and the user's language for %a %A
// %b %B. Anything else is kept as written.
[[nodiscard]] std::string formatClock(std::string_view format);

} // namespace rmw::barapp
