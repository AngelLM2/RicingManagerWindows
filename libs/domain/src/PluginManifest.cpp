#include "rmw/domain/PluginManifest.hpp"

#include "BarReader.hpp"
#include "EnumNames.hpp"
#include "TomlReader.hpp"

#include "rmw/domain/Color.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/Tweaks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace rmw::domain
{

using namespace detail;

namespace
{

constexpr auto kKindNames = std::to_array<EnumName<PluginKind>>({
    {PluginKind::BarWidget, "bar.widget"},
    {PluginKind::BarSkin, "bar.skin"},
    {PluginKind::Tweak, "tweak"},
    {PluginKind::Action, "action"},
    {PluginKind::Rule, "rule"},
});

constexpr auto kConfigTypeNames = std::to_array<EnumName<ConfigType>>({
    {ConfigType::Bool, "bool"},
    {ConfigType::Int, "int"},
    {ConfigType::Float, "float"},
    {ConfigType::Color, "color"},
    {ConfigType::Choice, "choice"},
    {ConfigType::String, "string"},
});

constexpr auto kWidgetTypeNames = std::to_array<EnumName<WidgetType>>({
    {WidgetType::String, "string"},
    {WidgetType::Int, "int"},
    {WidgetType::Float, "float"},
    {WidgetType::Bool, "bool"},
    {WidgetType::Color, "color"},
    {WidgetType::Image, "image"},
    {WidgetType::Length, "length"},
});

constexpr auto kRegistryTypeNames = std::to_array<EnumName<RegistryType>>({
    {RegistryType::Dword, "dword"},
    {RegistryType::String, "string"},
});

constexpr std::size_t kMaxAuthors = 16;
constexpr std::size_t kMaxKinds = 5;
constexpr std::size_t kMaxPermissionEntries = 32;
constexpr std::size_t kMaxNetworkDomains = 16;
constexpr std::size_t kMaxConfigFields = 64;
constexpr std::size_t kMaxChoices = 32;
constexpr std::size_t kMaxTweaks = 64;
constexpr std::size_t kMaxActions = 32;
constexpr std::size_t kMaxWidgetProperties = 32;
constexpr std::size_t kMaxWidgetStructs = 8;
constexpr std::size_t kMaxStructFields = 16;
constexpr std::size_t kMaxCallbacks = 16;
constexpr std::size_t kMaxCallbackArguments = 4;
constexpr std::size_t kHomepageMaxBytes = 256;
constexpr std::size_t kStringSettingMaxBytes = limits::kLongTextMaxBytes;
constexpr std::int64_t kDwordMax = 0xFFFF'FFFF;

// The bar gives every widget these two (rmw::bar::generateBarSource).
constexpr auto kBarProvidedProperties = std::to_array<std::string_view>({"foreground", "vertical"});

constexpr std::string_view kPalettePrefix = "palette.";

// A Slint type name the plugin declares: "NowPlaying", "Entry".
bool isTypeName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 32 || name.front() < 'A' || name.front() > 'Z')
    {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

// A settings key as written in rice.toml: "show_artist".
bool isConfigKey(std::string_view key) noexcept
{
    if (key.empty() || key.size() > 32 || key.front() < 'a' || key.front() > 'z')
    {
        return false;
    }
    return std::ranges::all_of(
        key, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool isColorText(std::string_view text) noexcept
{
    if (text.starts_with(kPalettePrefix))
    {
        return isValidPaletteName(text.substr(kPalettePrefix.size()));
    }
    return Color::parse(text).has_value();
}

std::string typeChoices()
{
    return std::format("{}, [<type>], <Struct> or [<Struct>]", enumChoices<WidgetType>(kWidgetTypeNames));
}

// What a settings value must be, for messages.
std::string expectation(const ConfigField& field)
{
    switch (field.type)
    {
    case ConfigType::Bool:
        return "true or false";
    case ConfigType::Int:
    case ConfigType::Float: {
        std::string text = field.type == ConfigType::Int ? "an integer" : "a number";
        if (field.min && field.max)
        {
            text += std::format(" from {} to {}", *field.min, *field.max);
        }
        else if (field.min)
        {
            text += std::format(" ≥ {}", *field.min);
        }
        else if (field.max)
        {
            text += std::format(" ≤ {}", *field.max);
        }
        return text;
    }
    case ConfigType::Color:
        return R"("#RRGGBB", "#RRGGBBAA" or "palette.<name>")";
    case ConfigType::Choice: {
        std::string text = "one of";
        for (std::size_t i = 0; i < field.choices.size(); ++i)
        {
            text += std::format("{} \"{}\"", i == 0 ? "" : ",", field.choices[i]);
        }
        return text;
    }
    case ConfigType::String:
        return std::format("text of at most {} bytes", field.maxLength);
    }
    return "?";
}

bool inRange(const ConfigField& field, double value) noexcept
{
    return (!field.min || value >= *field.min) && (!field.max || value <= *field.max);
}

// `value` as the field's type, or nullopt when it does not fit.
std::optional<Value> coerce(const ConfigField& field, const Value& value)
{
    switch (field.type)
    {
    case ConfigType::Bool:
        if (std::holds_alternative<bool>(value.data))
        {
            return value;
        }
        return std::nullopt;
    case ConfigType::Int:
        if (const auto* number = std::get_if<std::int64_t>(&value.data);
            number != nullptr && inRange(field, static_cast<double>(*number)))
        {
            return value;
        }
        return std::nullopt;
    case ConfigType::Float: {
        double number = 0.0;
        if (const auto* integer = std::get_if<std::int64_t>(&value.data))
        {
            number = static_cast<double>(*integer);
        }
        else if (const auto* real = std::get_if<double>(&value.data))
        {
            number = *real;
        }
        else
        {
            return std::nullopt;
        }
        if (!std::isfinite(number) || !inRange(field, number))
        {
            return std::nullopt;
        }
        return Value{number};
    }
    case ConfigType::Color:
        if (const auto* text = std::get_if<std::string>(&value.data); text != nullptr && isColorText(*text))
        {
            return value;
        }
        return std::nullopt;
    case ConfigType::Choice:
        if (const auto* text = std::get_if<std::string>(&value.data);
            text != nullptr && std::ranges::find(field.choices, *text) != field.choices.end())
        {
            return value;
        }
        return std::nullopt;
    case ConfigType::String:
        if (const auto* text = std::get_if<std::string>(&value.data);
            text != nullptr && text->size() <= field.maxLength && isDisplaySafeText(*text, false))
        {
            return value;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

class ManifestReader
{
public:
    ManifestReader(Diagnostics& diagnostics, const PluginParseOptions& options, PluginManifest& manifest)
        : m_diagnostics(diagnostics)
        , m_options(options)
        , m_manifest(manifest)
    {
    }

    void read(const toml::table& root)
    {
        TableReader t(root, {}, m_diagnostics);
        readSchema(t);
        if (const auto* plugin = t.table("plugin"))
        {
            readPlugin(*plugin);
        }
        else if (!t.raw().contains("plugin"))
        {
            t.missing("plugin");
        }
        if (const auto* permissions = t.table("permissions"))
        {
            readPermissions(*permissions);
        }
        if (const auto* entry = t.table("entry"))
        {
            readEntry(*entry);
        }
        if (const auto* config = t.table("config"))
        {
            readConfig(*config);
        }
        if (const auto* widget = t.table("widget"))
        {
            readWidget(*widget);
        }
        if (const auto* skin = t.table("skin"))
        {
            BarConfig bar;
            BarTableReader(m_diagnostics, nullptr).read(*skin, "skin", bar, true);
            if (bar.keys.empty())
            {
                t.error("skin", "a skin sets at least one key of a [[bar]] (thickness, background, left...)");
            }
            m_manifest.skin = std::move(bar);
        }
        readTweaks(t);
        readActions(t);
        t.reportUnknownKeys();
        checkKinds(t);
    }

private:
    // ---- top level ----------------------------------------------------------------------------

    void readSchema(TableReader& t)
    {
        const toml::node* node = t.get("schema");
        if (node == nullptr)
        {
            m_diagnostics.error(
                "schema", std::format("missing 'schema = {}' at the top of the file", kPluginSchemaVersion));
            return;
        }
        const auto* value = node->as_integer();
        if (value == nullptr)
        {
            t.error("schema", std::format("expected an integer, found {}", typeName(*node)));
            return;
        }
        if (value->get() > kPluginSchemaVersion)
        {
            t.error("schema",
                    std::format("this plugin uses format {}; update Ricing Manager to read it (this "
                                "version reads format {})",
                                value->get(), kPluginSchemaVersion));
        }
        else if (value->get() < kPluginSchemaVersion)
        {
            t.error("schema",
                    std::format("unknown format {} (expected {})", value->get(), kPluginSchemaVersion));
        }
    }

    void readPlugin(const toml::table& table)
    {
        TableReader t(table, "plugin", m_diagnostics);
        PluginManifest& m = m_manifest;

        if (auto id = t.string("id"))
        {
            if (isValidPackageId(*id))
            {
                m.id = std::move(*id);
            }
            else
            {
                t.error(
                    "id",
                    std::format("invalid id '{}' (expected reverse-DNS like io.github.you.my-widget)", *id));
            }
        }
        else if (!table.contains("id"))
        {
            t.missing("id");
        }

        if (auto name = t.string("name"))
        {
            if (name->empty())
            {
                t.error("name", "name cannot be empty");
            }
            m.name = std::move(*name);
        }
        else if (!table.contains("name"))
        {
            t.missing("name");
        }

        if (auto version = t.parsed("version", SemVer::parse))
        {
            m.version = std::move(*version);
        }
        else if (!table.contains("version"))
        {
            t.missing("version");
        }

        if (auto api = t.parsed("api", VersionReq::parse))
        {
            m.api = std::move(*api);
            if (!m.api.matches(m_options.apiVersion))
            {
                t.error("api", std::format("written for plugin API {}; this Ricing Manager has API {}",
                                           m.api.toString(), m_options.apiVersion.toString()));
            }
        }
        else if (!table.contains("api"))
        {
            t.missing("api");
        }

        readStringList(t, "authors", kMaxAuthors, m.authors);
        m.license = t.string("license").value_or("");
        if (auto homepage = t.string("homepage", kHomepageMaxBytes))
        {
            if (!homepage->starts_with("https://") || homepage->find_first_of(" \"<>") != std::string::npos)
            {
                t.error("homepage", "expected an https:// address");
            }
            else
            {
                m.homepage = std::move(*homepage);
            }
        }
        m.description = t.string("description", limits::kLongTextMaxBytes).value_or("");
        readKinds(t);
        m.minApp = t.parsed("min_app", SemVer::parse);
        if (m.minApp && *m.minApp > m_options.appVersion)
        {
            t.error("min_app", std::format("this plugin needs Ricing Manager {} or newer (this is {})",
                                           m.minApp->toString(), m_options.appVersion.toString()));
        }
        t.reportUnknownKeys();
    }

    void readKinds(TableReader& t)
    {
        const toml::node* node = t.get("kind");
        if (node == nullptr)
        {
            t.missing("kind");
            return;
        }
        std::vector<std::string> names;
        if (const auto* single = node->as_string())
        {
            names.push_back(single->get());
        }
        else if (t.array("kind", kMaxKinds) != nullptr)
        {
            for (const auto& item : *node->as_array())
            {
                const auto* text = item.as_string();
                if (text == nullptr)
                {
                    t.error("kind", "expected a list of kinds");
                    return;
                }
                names.push_back(text->get());
            }
        }
        else
        {
            return; // array() reported it
        }
        for (const auto& name : names)
        {
            const auto kind = enumFromName<PluginKind>(kKindNames, name);
            if (!kind)
            {
                t.error("kind", std::format("unknown kind '{}' (expected {})", name,
                                            enumChoices<PluginKind>(kKindNames)));
                continue;
            }
            if (!m_manifest.hasKind(*kind))
            {
                m_manifest.kinds.push_back(*kind);
            }
        }
        if (names.empty())
        {
            t.error("kind", "a plugin has at least one kind");
        }
    }

    void readPermissions(const toml::table& table)
    {
        TableReader t(table, "permissions", m_diagnostics);
        PluginPermissions& p = m_manifest.permissions;
        readNames(t, "providers", p.providers, findPluginProvider, pluginProviders());
        readNames(t, "actions", p.actions, findPluginAction, pluginActions());
        std::vector<std::string> domains;
        readStringList(t, "network", kMaxNetworkDomains, domains);
        for (auto& domain : domains)
        {
            if (!isValidNetworkDomain(domain))
            {
                t.error("network",
                        std::format("'{}' is not a domain plugins may reach (expected a public name "
                                    "like \"api.example.com\" or \"*.example.com\"; no IP addresses "
                                    "or local names)",
                                    domain));
                continue;
            }
            if (std::ranges::find(p.network, domain) == p.network.end())
            {
                p.network.push_back(std::move(domain));
            }
        }
        p.storage = t.boolean("storage").value_or(false);
        t.reportUnknownKeys();
    }

    void readEntry(const toml::table& table)
    {
        TableReader t(table, "entry", m_diagnostics);
        if (auto script = readPath(t, "script", "luau"))
        {
            m_manifest.script = std::move(*script);
        }
        if (table.contains("ui"))
        {
            t.get("ui");
            t.error("ui", "a widget's interface goes in [widget] ui = \"widget.slint\"");
        }
        t.reportUnknownKeys();
    }

    // ---- [config] -------------------------------------------------------------------------------

    void readConfig(const toml::table& table)
    {
        TableReader t(table, "config", m_diagnostics);
        if (table.size() > kMaxConfigFields)
        {
            m_diagnostics.error(
                "config", std::format("too many settings ({}, limit {})", table.size(), kMaxConfigFields));
            return;
        }
        for (const auto& [key, node] : table)
        {
            const std::string_view name = key.str();
            t.get(name);
            if (!isConfigKey(name))
            {
                t.error(name, "a setting name is lower-case letters, digits and '_', starting with a letter");
                continue;
            }
            const auto* field = node.as_table();
            if (field == nullptr)
            {
                t.error(name, R"(expected { type = "bool", default = true, label = "..." })");
                continue;
            }
            if (auto parsed = readConfigField(*field, t.pathOf(name), name))
            {
                m_manifest.config.fields.push_back(std::move(*parsed));
            }
        }
    }

    std::optional<ConfigField> readConfigField(const toml::table& table, const std::string& path,
                                               std::string_view key)
    {
        TableReader t(table, path, m_diagnostics);
        ConfigField field;
        field.key = std::string(key);
        const auto type = t.enumeration<ConfigType>("type", kConfigTypeNames);
        if (!type)
        {
            if (!table.contains("type"))
            {
                t.missing("type");
            }
            t.reportUnknownKeys();
            return std::nullopt;
        }
        field.type = *type;
        field.label = t.string("label").value_or(field.key);
        field.description = t.string("description", limits::kLongTextMaxBytes).value_or("");
        bool ok = true;

        switch (field.type)
        {
        case ConfigType::Bool:
            field.defaultValue = Value{t.boolean("default").value_or(false)};
            break;
        case ConfigType::Int: {
            constexpr auto kLow = std::numeric_limits<std::int32_t>::min();
            constexpr auto kHigh = std::numeric_limits<std::int32_t>::max();
            const auto min = t.integer("min", kLow, kHigh);
            const auto max = t.integer("max", kLow, kHigh);
            field.min = min ? std::optional<double>(static_cast<double>(*min)) : std::nullopt;
            field.max = max ? std::optional<double>(static_cast<double>(*max)) : std::nullopt;
            const std::int64_t low = min.value_or(kLow);
            const std::int64_t high = max.value_or(kHigh);
            const std::int64_t fallback = low <= high ? std::clamp<std::int64_t>(0, low, high) : low;
            field.defaultValue = Value{t.integer("default", low, high).value_or(fallback)};
            break;
        }
        case ConfigType::Float: {
            constexpr double kLimit = 1e9;
            field.min = t.number("min", -kLimit, kLimit);
            field.max = t.number("max", -kLimit, kLimit);
            const double low = field.min.value_or(-kLimit);
            const double high = field.max.value_or(kLimit);
            const double fallback = low <= high ? std::clamp(0.0, low, high) : low;
            field.defaultValue = Value{t.number("default", low, high).value_or(fallback)};
            break;
        }
        case ConfigType::Color:
            if (auto text = t.string("default"))
            {
                if (!isColorText(*text))
                {
                    t.error("default", R"(expected "#RRGGBB", "#RRGGBBAA" or "palette.<name>")");
                    ok = false;
                }
                field.defaultValue = Value{std::move(*text)};
            }
            else
            {
                if (!table.contains("default"))
                {
                    t.missing("default");
                }
                ok = false;
            }
            break;
        case ConfigType::Choice:
            readStringList(t, "choices", kMaxChoices, field.choices);
            if (field.choices.empty())
            {
                if (!table.contains("choices"))
                {
                    t.missing("choices");
                }
                ok = false;
                break;
            }
            if (auto text = t.string("default"))
            {
                if (std::ranges::find(field.choices, *text) == field.choices.end())
                {
                    t.error("default", std::format("'{}' is not one of the choices", *text));
                    ok = false;
                }
                field.defaultValue = Value{std::move(*text)};
            }
            else
            {
                field.defaultValue = Value{field.choices.front()};
            }
            break;
        case ConfigType::String:
            if (const auto length =
                    t.integer("max_length", 1, static_cast<std::int64_t>(kStringSettingMaxBytes)))
            {
                field.maxLength = static_cast<std::size_t>(*length);
            }
            if (auto text = t.string("default", field.maxLength))
            {
                field.defaultValue = Value{std::move(*text)};
            }
            else
            {
                field.defaultValue = Value{std::string()};
            }
            break;
        }
        if (field.min && field.max && *field.min > *field.max)
        {
            t.error("max", "max is smaller than min");
            ok = false;
        }
        t.reportUnknownKeys();
        if (!ok)
        {
            return std::nullopt;
        }
        return field;
    }

    // ---- [widget] -------------------------------------------------------------------------------

    void readWidget(const toml::table& table)
    {
        TableReader t(table, "widget", m_diagnostics);
        PluginWidget widget;
        if (auto ui = readPath(t, "ui", "slint"))
        {
            widget.ui = std::move(*ui);
        }
        else if (!table.contains("ui"))
        {
            t.missing("ui");
        }
        widget.component = readTypeName(t, "component").value_or("");
        if (widget.component.empty() && !table.contains("component"))
        {
            t.missing("component");
        }
        widget.verticalComponent = readTypeName(t, "vertical_component").value_or("");

        if (const auto* structs = t.table("structs"))
        {
            readStructs(*structs, t.pathOf("structs"), widget);
        }
        if (const auto* properties = t.table("properties"))
        {
            readProperties(*properties, t.pathOf("properties"), widget);
        }
        if (const auto* callbacks = t.table("callbacks"))
        {
            readCallbacks(*callbacks, t.pathOf("callbacks"), widget);
        }
        t.reportUnknownKeys();
        m_manifest.widget = std::move(widget);
    }

    std::optional<std::string> readTypeName(TableReader& t, std::string_view key)
    {
        auto name = t.string(key);
        if (name && !isTypeName(*name))
        {
            t.error(key,
                    std::format("'{}' is not a component name (a capital letter, then letters and digits)",
                                *name));
            return std::nullopt;
        }
        return name;
    }

    void readStructs(const toml::table& table, const std::string& path, PluginWidget& widget)
    {
        TableReader t(table, path, m_diagnostics);
        if (table.size() > kMaxWidgetStructs)
        {
            m_diagnostics.error(path, std::format("too many structs (limit {})", kMaxWidgetStructs));
            return;
        }
        for (const auto& [key, node] : table)
        {
            const std::string_view name = key.str();
            t.get(name);
            const auto* fields = node.as_table();
            if (!isTypeName(name) || fields == nullptr)
            {
                t.error(name, R"(expected [widget.structs.Name] with field = "type" entries)");
                continue;
            }
            if (fields->size() > kMaxStructFields || fields->empty())
            {
                t.error(name, std::format("a struct has 1 to {} fields", kMaxStructFields));
                continue;
            }
            WidgetStruct declared{std::string(name), {}};
            TableReader f(*fields, t.pathOf(name), m_diagnostics);
            for (const auto& [fieldKey, fieldNode] : *fields)
            {
                const std::string_view fieldName = fieldKey.str();
                if (!isValidPluginMemberName(fieldName))
                {
                    f.get(fieldName);
                    f.error(fieldName, "a field name is lower-case letters, digits and '-'");
                    continue;
                }
                if (const auto type = f.enumeration<WidgetType>(fieldName, kWidgetTypeNames))
                {
                    declared.fields.push_back({std::string(fieldName), *type});
                }
            }
            widget.structs.push_back(std::move(declared));
        }
    }

    void readProperties(const toml::table& table, const std::string& path, PluginWidget& widget)
    {
        TableReader t(table, path, m_diagnostics);
        if (table.size() > kMaxWidgetProperties)
        {
            m_diagnostics.error(path, std::format("too many properties (limit {})", kMaxWidgetProperties));
            return;
        }
        for (const auto& [key, node] : table)
        {
            const std::string_view name = key.str();
            if (!checkMemberName(t, name))
            {
                continue;
            }
            const auto spec = t.string(name);
            if (!spec)
            {
                continue;
            }
            if (auto type = parseTypeRef(*spec, widget))
            {
                widget.properties.push_back({std::string(name), std::move(*type)});
            }
            else
            {
                t.error(name, std::format("unknown type '{}' (expected {})", *spec, typeChoices()));
            }
        }
    }

    void readCallbacks(const toml::table& table, const std::string& path, PluginWidget& widget)
    {
        TableReader t(table, path, m_diagnostics);
        if (table.size() > kMaxCallbacks)
        {
            m_diagnostics.error(path, std::format("too many callbacks (limit {})", kMaxCallbacks));
            return;
        }
        for (const auto& [key, node] : table)
        {
            const std::string_view name = key.str();
            if (!checkMemberName(t, name))
            {
                continue;
            }
            if (std::ranges::find(widget.properties, name, &WidgetProperty::name) != widget.properties.end())
            {
                t.error(name, "a property has this name already");
                continue;
            }
            const auto* arguments = t.array(name, kMaxCallbackArguments);
            if (arguments == nullptr)
            {
                continue;
            }
            WidgetCallback callback{std::string(name), {}};
            bool ok = true;
            for (const auto& argument : *arguments)
            {
                const auto* text = argument.as_string();
                const auto type =
                    text != nullptr ? enumFromName<WidgetType>(kWidgetTypeNames, text->get()) : std::nullopt;
                if (!type || (*type != WidgetType::String && *type != WidgetType::Int &&
                              *type != WidgetType::Float && *type != WidgetType::Bool))
                {
                    t.error(name, R"(callback arguments are "string", "int", "float" or "bool")");
                    ok = false;
                    break;
                }
                callback.arguments.push_back(*type);
            }
            if (ok)
            {
                widget.callbacks.push_back(std::move(callback));
            }
        }
    }

    bool checkMemberName(TableReader& t, std::string_view name)
    {
        t.get(name);
        if (!isValidPluginMemberName(name))
        {
            t.error(name, "a name is lower-case letters, digits and '-', starting with a letter");
            return false;
        }
        if (std::ranges::find(kBarProvidedProperties, name) != kBarProvidedProperties.end())
        {
            t.error(name,
                    std::format("'{}' comes from the bar: declare it in the component, not here", name));
            return false;
        }
        return true;
    }

    static std::optional<WidgetTypeRef> parseTypeRef(std::string_view spec, const PluginWidget& widget)
    {
        WidgetTypeRef ref;
        if (spec.size() >= 2 && spec.front() == '[' && spec.back() == ']')
        {
            ref.array = true;
            spec = spec.substr(1, spec.size() - 2);
        }
        if (const auto type = enumFromName<WidgetType>(kWidgetTypeNames, spec))
        {
            ref.type = *type;
            return ref;
        }
        if (widget.findStruct(spec) != nullptr)
        {
            ref.structName = std::string(spec);
            return ref;
        }
        return std::nullopt;
    }

    // ---- [[tweak]] and [[action]] ---------------------------------------------------------------

    void readTweaks(TableReader& root)
    {
        const auto* tweaks = root.array("tweak", kMaxTweaks);
        if (tweaks == nullptr)
        {
            return;
        }
        std::size_t index = 0;
        for (const auto& node : *tweaks)
        {
            const std::string path = indexPath("tweak", index++);
            const auto* table = node.as_table();
            if (table == nullptr)
            {
                errorAt(m_diagnostics, node, path, "expected a table ([[tweak]])");
                continue;
            }
            if (auto tweak = readTweak(*table, path))
            {
                const bool duplicate = std::ranges::any_of(m_manifest.tweaks, [&](const Tweak& other) {
                    return other.key == tweak->key && other.value == tweak->value;
                });
                if (duplicate)
                {
                    errorAt(m_diagnostics, node, path, "this value is set twice");
                    continue;
                }
                m_manifest.tweaks.push_back(std::move(*tweak));
            }
        }
    }

    std::optional<Tweak> readTweak(const toml::table& table, const std::string& path)
    {
        TableReader t(table, path, m_diagnostics);
        Tweak tweak;
        bool ok = true;
        if (const auto key = t.string("key", 300))
        {
            if (auto valid = validateTweakKey(*key))
            {
                tweak.key = std::move(*valid);
            }
            else
            {
                t.error("key", valid.error().message());
                ok = false;
            }
        }
        else
        {
            if (!table.contains("key"))
            {
                t.missing("key");
            }
            ok = false;
        }
        if (auto name = t.string("value", 300))
        {
            if (ok)
            {
                if (auto valid = validateTweakValueName(tweak.key, *name); !valid)
                {
                    t.error("value", valid.error().message());
                    ok = false;
                }
            }
            tweak.value = std::move(*name);
        }
        else
        {
            if (!table.contains("value"))
            {
                t.missing("value");
            }
            ok = false;
        }
        const auto type = t.enumeration<RegistryType>("type", kRegistryTypeNames);
        if (!type)
        {
            if (!table.contains("type"))
            {
                t.missing("type");
            }
            ok = false;
        }
        else
        {
            tweak.type = *type;
            if (*type == RegistryType::Dword)
            {
                if (const auto data = t.integer("data", 0, kDwordMax))
                {
                    tweak.data = static_cast<std::uint64_t>(*data);
                }
                else
                {
                    if (!table.contains("data"))
                    {
                        t.missing("data");
                    }
                    ok = false;
                }
            }
            else if (auto data = t.string("data", 300))
            {
                if (auto valid = validateTweakString(*data); !valid)
                {
                    t.error("data", valid.error().message());
                    ok = false;
                }
                tweak.data = std::move(*data);
            }
            else
            {
                if (!table.contains("data"))
                {
                    t.missing("data");
                }
                ok = false;
            }
        }
        tweak.needsExplorerRestart = t.boolean("restart_explorer").value_or(false);
        t.reportUnknownKeys();
        if (!ok)
        {
            return std::nullopt;
        }
        return tweak;
    }

    void readActions(TableReader& root)
    {
        const auto* actions = root.array("action", kMaxActions);
        if (actions == nullptr)
        {
            return;
        }
        std::size_t index = 0;
        for (const auto& node : *actions)
        {
            const std::string path = indexPath("action", index++);
            const auto* table = node.as_table();
            if (table == nullptr)
            {
                errorAt(m_diagnostics, node, path, "expected a table ([[action]])");
                continue;
            }
            TableReader t(*table, path, m_diagnostics);
            PluginActionInfo action;
            if (auto name = t.string("name"))
            {
                if (!isValidPluginMemberName(*name))
                {
                    t.error("name", "an action name is lower-case letters, digits and '-'");
                }
                else if (m_manifest.findAction(*name) != nullptr)
                {
                    t.error("name", std::format("action '{}' is declared twice", *name));
                }
                else
                {
                    action.name = std::move(*name);
                }
            }
            else if (!table->contains("name"))
            {
                t.missing("name");
            }
            action.label = t.string("label").value_or(action.name);
            action.description = t.string("description", limits::kLongTextMaxBytes).value_or("");
            t.reportUnknownKeys();
            if (!action.name.empty())
            {
                m_manifest.actions.push_back(std::move(action));
            }
        }
    }

    // ---- consistency ----------------------------------------------------------------------------

    void checkKinds(TableReader& root)
    {
        const PluginManifest& m = m_manifest;
        const auto pair = [&](PluginKind kind, bool present, std::string_view key, std::string_view table) {
            const std::string_view name = enumToName<PluginKind>(kKindNames, kind);
            if (m.hasKind(kind) && !present)
            {
                m_diagnostics.error(std::string(key), std::format("kind \"{}\" needs {}", name, table));
            }
            else if (!m.hasKind(kind) && present)
            {
                m_diagnostics.error(std::string(key),
                                    std::format("{} is only read for kind \"{}\": add it to "
                                                "[plugin] kind",
                                                table, name));
            }
        };
        if (m.kinds.empty())
        {
            return; // reported with [plugin]
        }
        pair(PluginKind::BarWidget, m.widget.has_value(), "widget", "[widget]");
        pair(PluginKind::BarSkin, m.skin.has_value(), "skin", "[skin]");
        pair(PluginKind::Tweak, !m.tweaks.empty(), "tweak", "[[tweak]]");
        pair(PluginKind::Action, !m.actions.empty(), "action", "[[action]]");

        const bool runsScript =
            m.hasKind(PluginKind::BarWidget) || m.hasKind(PluginKind::Action) || m.hasKind(PluginKind::Rule);
        for (const PluginKind kind : {PluginKind::Action, PluginKind::Rule})
        {
            if (m.hasKind(kind) && !m.hasScript())
            {
                m_diagnostics.error("entry.script",
                                    std::format("kind \"{}\" needs a script: [entry] script = \"main.luau\"",
                                                enumToName<PluginKind>(kKindNames, kind)));
            }
        }
        if (m.hasScript() && !runsScript)
        {
            root.warning("entry",
                         "the script never runs: only bar.widget, action and rule plugins run scripts");
        }
        const auto& p = m.permissions;
        if (!m.hasScript() && (!p.providers.empty() || !p.actions.empty() || !p.network.empty() || p.storage))
        {
            root.warning("permissions", "permissions are for scripts, and this plugin has none");
        }
    }

    // ---- helpers --------------------------------------------------------------------------------

    std::optional<std::string> readPath(TableReader& t, std::string_view key, std::string_view extension)
    {
        const auto text = t.string(key, limits::kRelativePathMaxChars);
        if (!text)
        {
            return std::nullopt;
        }
        auto path = validatePackagePath(*text);
        if (!path)
        {
            t.error(key, path.error().message());
            return std::nullopt;
        }
        if (!hasExtension(*path, {extension}))
        {
            t.error(key, std::format("expected a .{} file", extension));
            return std::nullopt;
        }
        return std::move(*path);
    }

    void readStringList(TableReader& t, std::string_view key, std::size_t limit,
                        std::vector<std::string>& out)
    {
        const auto* array = t.array(key, limit);
        if (array == nullptr)
        {
            return;
        }
        for (const auto& node : *array)
        {
            const auto* text = node.as_string();
            if (text == nullptr || text->get().empty() || text->get().size() > limits::kShortTextMaxBytes ||
                !isDisplaySafeText(text->get(), false))
            {
                t.error(key, "expected a list of short texts");
                return;
            }
            out.push_back(text->get());
        }
    }

    template <class Find>
    void readNames(TableReader& t, std::string_view key, std::vector<std::string>& out, Find find,
                   std::span<const PluginCapability> known)
    {
        std::vector<std::string> names;
        readStringList(t, key, kMaxPermissionEntries, names);
        for (auto& name : names)
        {
            if (find(name) == nullptr)
            {
                std::string choices;
                for (const auto& capability : known)
                {
                    choices += choices.empty() ? "" : ", ";
                    choices += capability.name;
                }
                t.error(key, std::format("unknown name '{}' (this version knows {})", name, choices));
                continue;
            }
            if (std::ranges::find(out, name) == out.end())
            {
                out.push_back(std::move(name));
            }
        }
    }

    Diagnostics& m_diagnostics;
    const PluginParseOptions& m_options;
    PluginManifest& m_manifest;
};

} // namespace

std::string_view toString(PluginKind kind) noexcept
{
    return enumToName<PluginKind>(kKindNames, kind);
}

std::string_view toString(ConfigType type) noexcept
{
    return enumToName<ConfigType>(kConfigTypeNames, type);
}

std::string_view toString(WidgetType type) noexcept
{
    return enumToName<WidgetType>(kWidgetTypeNames, type);
}

std::string_view toString(RegistryType type) noexcept
{
    return enumToName<RegistryType>(kRegistryTypeNames, type);
}

std::string WidgetTypeRef::toString() const
{
    const std::string name = structName.empty() ? std::string(domain::toString(type)) : structName;
    return array ? std::format("[{}]", name) : name;
}

bool PluginPermissions::hasSensitive() const noexcept
{
    const auto sensitive = [](const PluginCapability* capability) {
        return capability != nullptr && capability->sensitive;
    };
    return !network.empty() || std::ranges::any_of(providers, [&](const std::string& n) {
        return sensitive(findPluginProvider(n));
    }) || std::ranges::any_of(actions, [&](const std::string& n) { return sensitive(findPluginAction(n)); });
}

bool PluginPermissions::isWithin(const PluginPermissions& granted) const noexcept
{
    const auto within = [](const std::vector<std::string>& asked, const std::vector<std::string>& allowed) {
        return std::ranges::all_of(asked, [&](const std::string& name) {
            return std::ranges::find(allowed, name) != allowed.end();
        });
    };
    return within(providers, granted.providers) && within(actions, granted.actions) &&
           within(network, granted.network) && (!storage || granted.storage);
}

PluginPermissions PluginPermissions::intersect(const PluginPermissions& other) const
{
    const auto both = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
        std::vector<std::string> result;
        for (const auto& name : a)
        {
            if (std::ranges::find(b, name) != b.end())
            {
                result.push_back(name);
            }
        }
        return result;
    };
    return {both(providers, other.providers), both(actions, other.actions), both(network, other.network),
            storage && other.storage};
}

const ConfigField* ConfigSchema::find(std::string_view key) const noexcept
{
    const auto found = std::ranges::find(fields, key, &ConfigField::key);
    return found == fields.end() ? nullptr : &*found;
}

const WidgetStruct* PluginWidget::findStruct(std::string_view name) const noexcept
{
    const auto found = std::ranges::find(structs, name, &WidgetStruct::name);
    return found == structs.end() ? nullptr : &*found;
}

bool PluginManifest::hasKind(PluginKind kind) const noexcept
{
    return std::ranges::find(kinds, kind) != kinds.end();
}

const PluginActionInfo* PluginManifest::findAction(std::string_view action) const noexcept
{
    const auto found = std::ranges::find(actions, action, &PluginActionInfo::name);
    return found == actions.end() ? nullptr : &*found;
}

Result<PluginManifest> parsePluginManifest(std::string_view text, Diagnostics& diagnostics,
                                           const PluginParseOptions& options)
{
    PluginManifest manifest;
    if (auto root = parseToml(text, diagnostics))
    {
        ManifestReader(diagnostics, options, manifest).read(*root);
    }
    if (diagnostics.hasErrors())
    {
        std::size_t errors = 0;
        const Diagnostic* first = nullptr;
        for (const auto& item : diagnostics.all())
        {
            if (item.severity == Severity::Error)
            {
                first = first != nullptr ? first : &item;
                ++errors;
            }
        }
        return fail(ErrorCode::InvalidData,
                    std::format("plugin.toml has {} error{}; first: {}", errors, errors == 1 ? "" : "s",
                                first->toString("plugin.toml")));
    }
    return manifest;
}

namespace
{

Result<void> checkScalar(WidgetType type, const Value& value)
{
    switch (type)
    {
    case WidgetType::String:
        if (const auto* text = std::get_if<std::string>(&value.data))
        {
            if (text->size() > kWidgetTextMaxBytes)
            {
                return fail(ErrorCode::LimitExceeded,
                            std::format("text longer than {} bytes", kWidgetTextMaxBytes));
            }
            return {};
        }
        return fail(ErrorCode::InvalidData, "expected text");
    case WidgetType::Int:
        if (std::holds_alternative<std::int64_t>(value.data))
        {
            return {};
        }
        return fail(ErrorCode::InvalidData, "expected an integer");
    case WidgetType::Float:
    case WidgetType::Length:
        if (std::holds_alternative<std::int64_t>(value.data) || std::holds_alternative<double>(value.data))
        {
            return {};
        }
        return fail(ErrorCode::InvalidData, "expected a number");
    case WidgetType::Bool:
        if (std::holds_alternative<bool>(value.data))
        {
            return {};
        }
        return fail(ErrorCode::InvalidData, "expected true or false");
    case WidgetType::Color:
        if (const auto* text = std::get_if<std::string>(&value.data); text != nullptr && Color::parse(*text))
        {
            return {};
        }
        return fail(ErrorCode::InvalidData, R"(expected a color "#RRGGBB" or "#RRGGBBAA")");
    case WidgetType::Image: {
        const auto* text = std::get_if<std::string>(&value.data);
        if (text == nullptr)
        {
            return fail(ErrorCode::InvalidData, "expected an image path");
        }
        if (text->empty())
        {
            return {};
        }
        if (text->starts_with("window:"))
        {
            const std::string_view id = std::string_view(*text).substr(7);
            if (!id.empty() && id.size() <= 20 &&
                std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; }))
            {
                return {};
            }
            return fail(ErrorCode::InvalidData, "expected \"window:<id>\"");
        }
        auto path = validatePackagePath(*text);
        if (!path)
        {
            return std::unexpected(std::move(path.error()));
        }
        if (!hasExtension(*path, {"png", "jpg", "jpeg", "svg"}))
        {
            return fail(ErrorCode::InvalidData, "expected a .png, .jpg or .svg file");
        }
        return {};
    }
    }
    return fail(ErrorCode::InvalidData, "unknown type");
}

Result<void> checkItem(const PluginWidget& widget, const WidgetTypeRef& type, const Value& value)
{
    if (type.structName.empty())
    {
        return checkScalar(type.type, value);
    }
    const WidgetStruct* declared = widget.findStruct(type.structName);
    const auto* members = value.table();
    if (declared == nullptr || members == nullptr)
    {
        return fail(ErrorCode::InvalidData, std::format("expected a table ({})", type.structName));
    }
    for (const auto& member : *members)
    {
        const auto field = std::ranges::find(declared->fields, member.key, &WidgetField::name);
        if (field == declared->fields.end())
        {
            return fail(ErrorCode::InvalidData,
                        std::format("{} has no field '{}'", type.structName, member.key));
        }
        if (auto checked = checkScalar(field->type, member.value); !checked)
        {
            return std::unexpected(std::move(checked.error()).withContext(member.key));
        }
    }
    return {};
}

} // namespace

Result<void> checkWidgetValue(const PluginWidget& widget, const WidgetTypeRef& type, const Value& value)
{
    if (!type.array)
    {
        return checkItem(widget, type, value);
    }
    const auto* items = std::get_if<Value::Array>(&value.data);
    if (items == nullptr)
    {
        // An empty Lua table has no way to say it is a list.
        if (const auto* members = value.table(); members != nullptr && members->empty())
        {
            return {};
        }
        return fail(ErrorCode::InvalidData, "expected a list");
    }
    if (items->size() > kWidgetListMaxItems)
    {
        return fail(ErrorCode::LimitExceeded, std::format("more than {} items", kWidgetListMaxItems));
    }
    for (std::size_t i = 0; i < items->size(); ++i)
    {
        if (auto checked = checkItem(widget, type, (*items)[i]); !checked)
        {
            return std::unexpected(std::move(checked.error()).withContext(std::format("item {}", i + 1)));
        }
    }
    return {};
}

Value resolvePluginConfig(const ConfigSchema& schema, std::span<const Value* const> layers,
                          Diagnostics& diagnostics, std::string_view where)
{
    Value::Table result;
    for (const auto& field : schema.fields)
    {
        result.push_back({field.key, field.defaultValue});
    }
    for (const Value* layer : layers)
    {
        if (layer == nullptr || layer->isNull())
        {
            continue;
        }
        const auto* members = layer->table();
        if (members == nullptr)
        {
            diagnostics.warning(std::string(where), "expected a table of settings (ignored)");
            continue;
        }
        for (const auto& member : *members)
        {
            const std::string key = std::format("{}.{}", where, member.key);
            const ConfigField* field = schema.find(member.key);
            if (field == nullptr)
            {
                diagnostics.warning(key, "this plugin has no such setting (ignored)");
                continue;
            }
            auto value = coerce(*field, member.value);
            if (!value)
            {
                diagnostics.warning(key,
                                    std::format("expected {} (the default is used)", expectation(*field)));
                continue;
            }
            std::ranges::find(result, member.key, &ValueMember::key)->value = std::move(*value);
        }
    }
    return Value{std::move(result)};
}

} // namespace rmw::domain
