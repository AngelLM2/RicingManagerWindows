#pragma once

#include "Widgets.hpp"

#include "rmw/bar/Plan.hpp"
#include "rmw/bar/Source.hpp"
#include "rmw/bar/WidgetSource.hpp"
#include "rmw/domain/PluginManifest.hpp"

#include <slint-interpreter.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::barapp
{

// One plugin's widget, ready to compile into a bar: its manifest's [widget], its .slint checked
// and renamed (rmw::bar::checkWidgetSource), and the kind the generator places.
struct PluginWidget
{
    std::string id;
    std::filesystem::path folder;
    bool dev = false;
    domain::PluginWidget manifest;
    bar::CheckedWidgetSource checked;
    bar::WidgetKind kind;

    // What `kind` points into.
    std::vector<std::string> types;
    std::vector<bar::WidgetProperty> properties;
    std::vector<std::string> callbackText;
    std::vector<std::string_view> callbacks;
};

// The plugin widgets the plan names. Loaded again on every rebuild (a plugin being written shows
// its last saved state). A plugin whose files do not pass the checks is left out, with the reason
// in the log; the bar shows the rest.
class PluginWidgets
{
public:
    void load(const std::vector<bar::PlanPlugin>& plugins);

    [[nodiscard]] const PluginWidget* find(std::string_view id) const;
    [[nodiscard]] const std::vector<std::unique_ptr<PluginWidget>>& all() const noexcept { return m_widgets; }
    // The built-in kinds followed by the plugins'.
    [[nodiscard]] std::vector<bar::WidgetKind> kinds() const;
    // std-widgets imports first, then the built-in sources, then every plugin's.
    [[nodiscard]] std::string sources(std::string_view builtin) const;
    // The same with only `widget` of the plugins: the source to try it alone.
    [[nodiscard]] static std::string sourcesWith(const PluginWidget& widget, std::string_view builtin);
    // Where `id`'s source starts in a generated bar source: the line before its first line (0 when
    // it is not there).
    [[nodiscard]] static std::size_t lineBefore(std::string_view generated, std::string_view id);
    // Leaves a widget out (Slint refused it); pointers to the others stay valid.
    void remove(std::string_view id);

    // A property value from the plugin host, as Slint wants it. Images are files of the plugin
    // (checked again here) or "window:<id>" (that window's icon); a bad value is an error.
    [[nodiscard]] Result<slint::interpreter::Value> convert(const PluginWidget& widget,
                                                            const domain::WidgetTypeRef& type,
                                                            const domain::Value& value);

private:
    [[nodiscard]] Result<slint::interpreter::Value> convertItem(const PluginWidget& widget,
                                                                const domain::WidgetTypeRef& type,
                                                                const domain::Value& value);
    [[nodiscard]] Result<slint::interpreter::Value> convertScalar(const PluginWidget& widget,
                                                                  domain::WidgetType type,
                                                                  const domain::Value& value);
    [[nodiscard]] Result<slint::Image> image(const PluginWidget& widget, std::string_view source);

    std::vector<std::unique_ptr<PluginWidget>> m_widgets;
    std::map<std::filesystem::path, slint::Image> m_images; // loaded files, by final path
};

} // namespace rmw::barapp
