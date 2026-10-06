#pragma once

#include "rmw/bar/Source.hpp"
#include "rmw/providers/Windows.hpp"

#include <slint-interpreter.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// Brings `model` to `rows`, row by row; `keys` names its rows now and becomes `next` (the rows' names, in the
// order of `rows`). A row that stays keeps its item, and with it a hover or an animation going on, and
// gets the new data; one that comes is inserted where it goes (and grows in, v1.8); one that goes is
// removed. Replacing the whole model made every button pop up again at every title change. When those that
// stay changed order, the rows are all made again.
template <typename Key>
void syncRows(slint::VectorModel<slint::interpreter::Value>& model, std::vector<Key>& keys, std::vector<Key> next,
              const std::vector<slint::interpreter::Value>& rows)
{
    for (std::size_t i = keys.size(); i-- > 0;)
    {
        if (std::ranges::find(next, keys[i]) == next.end())
        {
            model.erase(i);
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    std::vector<Key> staying;
    for (const Key& key : next)
    {
        if (std::ranges::find(keys, key) != keys.end())
        {
            staying.push_back(key);
        }
    }
    if (staying != keys)
    {
        for (std::size_t i = model.row_count(); i-- > 0;)
        {
            model.erase(i);
        }
        for (const auto& row : rows)
        {
            model.push_back(row);
        }
        keys = std::move(next);
        return;
    }
    for (std::size_t j = 0; j < next.size() && j < rows.size(); ++j)
    {
        if (j < keys.size() && keys[j] == next[j])
        {
            model.set_row_data(j, rows[j]);
        }
        else
        {
            model.insert(j, rows[j]);
            keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(j), next[j]);
        }
    }
}

// "%H:%M" with a safe, small set of conversions (a rice is untrusted: no strftime, whose invalid
// conversions end the process): %H %I %M %S %p %d %m %y %Y, and the user's language for %a %A
// %b %B. Anything else is kept as written.
[[nodiscard]] std::string formatClock(std::string_view format);

} // namespace rmw::barapp
