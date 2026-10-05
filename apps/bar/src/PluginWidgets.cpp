#include "PluginWidgets.hpp"

#include "TrayHost.hpp"
#include "WindowList.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/plugins/PluginFiles.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <span>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

std::string_view slintType(domain::WidgetType type)
{
    switch (type)
    {
    case domain::WidgetType::String:
        return "string";
    case domain::WidgetType::Int:
        return "int";
    case domain::WidgetType::Float:
        return "float";
    case domain::WidgetType::Bool:
        return "bool";
    case domain::WidgetType::Color:
        return "color";
    case domain::WidgetType::Image:
        return "image";
    case domain::WidgetType::Length:
        return "length";
    }
    return "string";
}

Result<std::string> readText(const std::filesystem::path& file, std::uint64_t maxBytes)
{
    auto bytes = platform::readFileLimited(file, maxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

bool declares(const std::vector<std::string>& names, std::string_view name)
{
    return std::ranges::find(names, name) != names.end();
}

// The manifest's properties and callbacks must exist on the component (and on the vertical one).
Result<void> matches(const domain::PluginWidget& manifest, const bar::WidgetInterface& interface,
                     std::string_view component)
{
    for (const auto& property : manifest.properties)
    {
        if (!declares(interface.inProperties, property.name))
        {
            return fail(ErrorCode::InvalidData,
                        std::format("{} has no `in property` '{}' (plugin.toml [widget.properties] names it)",
                                    component, property.name));
        }
    }
    for (const auto& callback : manifest.callbacks)
    {
        if (!declares(interface.callbacks, callback.name))
        {
            return fail(ErrorCode::InvalidData,
                        std::format("{} has no callback '{}' (plugin.toml [widget.callbacks] names it)",
                                    component, callback.name));
        }
    }
    return {};
}

Value defaultOf(domain::WidgetType type)
{
    switch (type)
    {
    case domain::WidgetType::String:
        return {slint::SharedString()};
    case domain::WidgetType::Int:
    case domain::WidgetType::Float:
    case domain::WidgetType::Length:
        return {0.0};
    case domain::WidgetType::Bool:
        return {false};
    case domain::WidgetType::Color:
        return {slint::Brush(slint::Color::from_argb_uint8(0, 0, 0, 0))};
    case domain::WidgetType::Image:
        return {slint::Image()};
    }
    return {};
}

std::string marker(std::string_view id)
{
    return std::format("// ---- plugin widget {} ----\n", id);
}

// std-widgets imports first, then the built-in sources, then the plugins'. Each plugin's source
// starts on the line after its marker, so Slint's line numbers map back to the plugin's file.
std::string joinSources(std::span<const PluginWidget* const> widgets, std::string_view builtin)
{
    std::vector<std::string> imports;
    for (const PluginWidget* widget : widgets)
    {
        for (const auto& name : widget->checked.stdWidgets)
        {
            if (!declares(imports, name))
            {
                imports.push_back(name);
            }
        }
    }
    std::string source;
    if (!imports.empty())
    {
        source = "import { ";
        for (std::size_t i = 0; i < imports.size(); ++i)
        {
            source += (i == 0 ? "" : ", ") + imports[i];
        }
        source += " } from \"std-widgets.slint\";\n";
    }
    source += builtin;
    for (const PluginWidget* widget : widgets)
    {
        source += '\n';
        source += marker(widget->id);
        source += widget->checked.source;
    }
    return source;
}

} // namespace

void PluginWidgets::load(const std::vector<bar::PlanPlugin>& plugins)
{
    m_widgets.clear();
    m_images.clear(); // a plugin being written may have new pictures
    int number = 0;
    for (const auto& plugin : plugins)
    {
        ++number;
        const auto skip = [&](std::string_view reason) {
            log::error("bar: plugin widget {} left out: {}", plugin.id, reason);
        };
        auto manifestText = readText(plugin.folder / L"plugin.toml", limits::kManifestMaxBytes);
        if (!manifestText)
        {
            skip(manifestText.error().message());
            continue;
        }
        domain::Diagnostics diagnostics;
        auto manifest = domain::parsePluginManifest(*manifestText, diagnostics, {});
        if (!manifest)
        {
            skip(manifest.error().message());
            continue;
        }
        if (manifest->id != plugin.id || !manifest->widget)
        {
            skip("the folder does not hold this plugin's widget");
            continue;
        }
        const domain::PluginWidget& declared = *manifest->widget;
        auto source = readText(plugin.folder / std::filesystem::path(utf8To16Lossy(declared.ui)),
                               bar::kWidgetSourceMaxBytes);
        if (!source)
        {
            skip(source.error().message());
            continue;
        }
        bar::WidgetSourceOptions options;
        options.prefix = std::format("Rmwp{}", number);
        options.component = declared.component;
        options.verticalComponent = declared.verticalComponent;
        options.resolveImage = [folder = plugin.folder](std::string_view path) -> Result<std::string> {
            auto file = plugins::pluginImageFile(folder, path);
            if (!file)
            {
                return std::unexpected(std::move(file.error()));
            }
            return utf16To8Lossy(file->generic_wstring());
        };
        auto checked = bar::checkWidgetSource(*source, declared.ui, options);
        if (!checked)
        {
            skip(checked.error().message());
            continue;
        }
        if (auto fits = matches(declared, checked->interface, declared.component); !fits)
        {
            skip(fits.error().message());
            continue;
        }
        if (!declared.verticalComponent.empty())
        {
            if (auto fits = matches(declared, checked->verticalInterface, declared.verticalComponent); !fits)
            {
                skip(fits.error().message());
                continue;
            }
        }

        auto widget = std::make_unique<PluginWidget>();
        widget->id = plugin.id;
        widget->folder = plugin.folder;
        widget->dev = plugin.dev;
        widget->manifest = declared;
        widget->checked = std::move(*checked);
        // Views into these must stay put: reserve first.
        widget->types.reserve(widget->manifest.properties.size());
        widget->properties.reserve(widget->manifest.properties.size());
        for (const auto& property : widget->manifest.properties)
        {
            const auto& ref = property.type;
            std::string inner = ref.structName.empty() ? std::string(slintType(ref.type))
                                                       : widget->checked.renamed(ref.structName);
            widget->types.push_back(ref.array ? std::format("[{}]", inner) : inner);
            widget->properties.push_back({property.name, widget->types.back()});
        }
        widget->callbackText.reserve(widget->manifest.callbacks.size());
        for (const auto& callback : widget->manifest.callbacks)
        {
            std::string text = callback.name;
            if (!callback.arguments.empty())
            {
                text += '(';
                for (std::size_t i = 0; i < callback.arguments.size(); ++i)
                {
                    text += i == 0 ? "" : ", ";
                    text += slintType(callback.arguments[i]);
                }
                text += ')';
            }
            widget->callbackText.push_back(std::move(text));
        }
        for (const auto& text : widget->callbackText)
        {
            widget->callbacks.emplace_back(text);
        }
        widget->kind.id = widget->id;
        widget->kind.component = widget->checked.component;
        widget->kind.verticalComponent = widget->checked.verticalComponent;
        widget->kind.properties = widget->properties;
        widget->kind.callbacks = widget->callbacks;
        widget->kind.takesForeground = declares(widget->checked.interface.inProperties, "foreground");
        widget->kind.takesVertical = declares(widget->checked.interface.inProperties, "vertical");
        log::info("bar: plugin widget {} ready", plugin.id);
        m_widgets.push_back(std::move(widget));
    }
}

const PluginWidget* PluginWidgets::find(std::string_view id) const
{
    for (const auto& widget : m_widgets)
    {
        if (widget->id == id)
        {
            return widget.get();
        }
    }
    return nullptr;
}

std::vector<bar::WidgetKind> PluginWidgets::kinds() const
{
    std::vector<bar::WidgetKind> kinds(builtinKinds().begin(), builtinKinds().end());
    for (const auto& widget : m_widgets)
    {
        kinds.push_back(widget->kind);
    }
    return kinds;
}

std::string PluginWidgets::sources(std::string_view builtin) const
{
    std::vector<const PluginWidget*> widgets;
    widgets.reserve(m_widgets.size());
    for (const auto& widget : m_widgets)
    {
        widgets.push_back(widget.get());
    }
    return joinSources(widgets, builtin);
}

std::string PluginWidgets::sourcesWith(const PluginWidget& widget, std::string_view builtin)
{
    const std::array<const PluginWidget*, 1> one{&widget};
    return joinSources(one, builtin);
}

std::size_t PluginWidgets::lineBefore(std::string_view generated, std::string_view id)
{
    const auto at = generated.find(marker(id));
    if (at == std::string_view::npos)
    {
        return 0;
    }
    // The marker's own line: the plugin's source starts on the next one.
    return static_cast<std::size_t>(std::ranges::count(generated.substr(0, at), '\n')) + 1;
}

void PluginWidgets::remove(std::string_view id)
{
    std::erase_if(m_widgets, [id](const auto& widget) { return widget->id == id; });
}

Result<Value> PluginWidgets::convert(const PluginWidget& widget, const domain::WidgetTypeRef& type,
                                     const domain::Value& value)
{
    if (auto checked = domain::checkWidgetValue(widget.manifest, type, value); !checked)
    {
        return std::unexpected(std::move(checked.error()));
    }
    if (!type.array)
    {
        return convertItem(widget, type, value);
    }
    auto model = std::make_shared<slint::VectorModel<Value>>();
    if (const auto* items = std::get_if<domain::Value::Array>(&value.data))
    {
        domain::WidgetTypeRef item = type;
        item.array = false;
        for (const auto& element : *items)
        {
            auto converted = convertItem(widget, item, element);
            if (!converted)
            {
                return std::unexpected(std::move(converted.error()));
            }
            model->push_back(*converted);
        }
    }
    return Value(std::shared_ptr<slint::Model<Value>>(model));
}

Result<Value> PluginWidgets::convertItem(const PluginWidget& widget, const domain::WidgetTypeRef& type,
                                         const domain::Value& value)
{
    if (type.structName.empty())
    {
        return convertScalar(widget, type.type, value);
    }
    const auto* declared = widget.manifest.findStruct(type.structName);
    if (declared == nullptr)
    {
        return fail(ErrorCode::InvalidData, "unknown struct");
    }
    slint::interpreter::Struct item;
    for (const auto& field : declared->fields)
    {
        const domain::Value* member = value.find(field.name);
        if (member == nullptr)
        {
            item.set_field(field.name, defaultOf(field.type));
            continue;
        }
        auto converted = convertScalar(widget, field.type, *member);
        if (!converted)
        {
            return std::unexpected(std::move(converted.error()));
        }
        item.set_field(field.name, *converted);
    }
    return Value(item);
}

Result<Value> PluginWidgets::convertScalar(const PluginWidget& widget, domain::WidgetType type,
                                           const domain::Value& value)
{
    switch (type)
    {
    case domain::WidgetType::String:
        return Value(slint::SharedString(std::get<std::string>(value.data)));
    case domain::WidgetType::Int:
    case domain::WidgetType::Float:
    case domain::WidgetType::Length:
        if (const auto* integer = std::get_if<std::int64_t>(&value.data))
        {
            return Value(static_cast<double>(*integer));
        }
        return Value(std::get<double>(value.data));
    case domain::WidgetType::Bool:
        return Value(std::get<bool>(value.data));
    case domain::WidgetType::Color: {
        auto color = domain::Color::parse(std::get<std::string>(value.data));
        if (!color)
        {
            return std::unexpected(std::move(color.error()));
        }
        return Value(slint::Brush(slint::Color::from_argb_uint8(color->a, color->r, color->g, color->b)));
    }
    case domain::WidgetType::Image: {
        auto loaded = image(widget, std::get<std::string>(value.data));
        if (!loaded)
        {
            return std::unexpected(std::move(loaded.error()));
        }
        return Value(*loaded);
    }
    }
    return fail(ErrorCode::InvalidData, "unknown type");
}

Result<slint::Image> PluginWidgets::image(const PluginWidget& widget, std::string_view source)
{
    if (source.empty())
    {
        return slint::Image();
    }
    if (source.starts_with("window:"))
    {
        std::uint64_t id = 0;
        const std::string_view digits = source.substr(7);
        std::from_chars(digits.data(), digits.data() + digits.size(), id);
        for (const auto& entry : WindowTracker::instance().windows())
        {
            if (reinterpret_cast<std::uintptr_t>(entry.window) == id)
            {
                return entry.icon;
            }
        }
        return slint::Image(); // gone, or not a taskbar window
    }
    if (source.starts_with("tray:"))
    {
        std::uint64_t number = 0;
        const std::string_view digits = source.substr(5);
        std::from_chars(digits.data(), digits.data() + digits.size(), number);
        if (const auto* view = TrayHost::instance().find(number))
        {
            return imageFromPixels(view->pixels);
        }
        return slint::Image(); // gone, or the tray is not watched
    }
    auto file = plugins::pluginImageFile(widget.folder, source);
    if (!file)
    {
        return std::unexpected(std::move(file.error()));
    }
    if (const auto cached = m_images.find(*file); cached != m_images.end())
    {
        return cached->second;
    }
    auto loaded = slint::Image::load_from_path(slint::SharedString(utf16To8Lossy(file->native())));
    m_images.emplace(*file, loaded);
    return loaded;
}

} // namespace rmw::barapp
