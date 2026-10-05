#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Diagnostics.hpp"
#include "rmw/domain/PluginApi.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/SemVer.hpp"
#include "rmw/domain/Value.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// plugin.toml (docs/12 §4): what a plugin is, what it may do and what it brings. Untrusted input,
// read with the same loader, limits and diagnostics as rice.toml.
namespace rmw::domain
{

inline constexpr int kPluginSchemaVersion = 1;

enum class PluginKind
{
    BarWidget, // "bar.widget": a .slint component for the bar, fed by its script (or static)
    BarSkin,   // "bar.skin": a preset of how a bar looks and what it holds (data only)
    Tweak,     // "tweak": allow-listed HKCU values, applied and undone like any module (P1)
    Action,    // "action": named actions hotkeys, rules and the CLI can call (script)
    Rule,      // "rule": a script that watches providers and calls actions on its own
};

std::string_view toString(PluginKind kind) noexcept;

struct PluginPermissions
{
    std::vector<std::string> providers; // pluginProviders() names
    std::vector<std::string> actions;   // pluginActions() names
    std::vector<std::string> network;   // domains (isValidNetworkDomain)
    bool storage = false;

    // Something the install prompt highlights (privacy or reach).
    [[nodiscard]] bool hasSensitive() const noexcept;
    // Every permission here is also in `granted` (an update that asks for more needs approval).
    [[nodiscard]] bool isWithin(const PluginPermissions& granted) const noexcept;
    // What both allow.
    [[nodiscard]] PluginPermissions intersect(const PluginPermissions& other) const;

    friend bool operator==(const PluginPermissions&, const PluginPermissions&) = default;
};

// ---- settings schema ([config]): the settings form is generated from it --------------------------

enum class ConfigType
{
    Bool,
    Int,
    Float,
    Color,  // "#RRGGBB", "#RRGGBBAA" or "palette.<name>"
    Choice, // one of `choices`
    String,
};

std::string_view toString(ConfigType type) noexcept;

struct ConfigField
{
    std::string key;
    ConfigType type = ConfigType::String;
    std::string label;
    std::string description;
    Value defaultValue; // always valid for the field
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> choices;
    std::size_t maxLength = 256; // String
};

struct ConfigSchema
{
    std::vector<ConfigField> fields;

    [[nodiscard]] const ConfigField* find(std::string_view key) const noexcept;
};

// ---- bar.widget ---------------------------------------------------------------------------------

// Types a widget property can have. The bar converts what the script sends to exactly these.
enum class WidgetType
{
    String,
    Int,
    Float,
    Bool,
    Color,
    Image,  // a file in the plugin's folder, or "window:<id>" (needs windows.list)
    Length, // logical pixels
};

std::string_view toString(WidgetType type) noexcept;

struct WidgetTypeRef
{
    WidgetType type = WidgetType::String; // when structName is empty
    std::string structName;               // a struct of [widget.structs]
    bool array = false;                   // "[T]": a list (a Slint model)

    // As written in the manifest and in Slint: "string", "[Entry]".
    [[nodiscard]] std::string toString() const;
    friend bool operator==(const WidgetTypeRef&, const WidgetTypeRef&) = default;
};

struct WidgetField
{
    std::string name;
    WidgetType type = WidgetType::String;
};

struct WidgetStruct
{
    std::string name; // as in the .slint: "Entry"
    std::vector<WidgetField> fields;
};

struct WidgetProperty
{
    std::string name;
    WidgetTypeRef type;
};

struct WidgetCallback
{
    std::string name;
    std::vector<WidgetType> arguments; // String, Int, Float or Bool
};

struct PluginWidget
{
    std::string ui;                // package path of the .slint file
    std::string component;         // its exported component
    std::string verticalComponent; // for vertical bars; empty = the same
    std::vector<WidgetStruct> structs;
    std::vector<WidgetProperty> properties;
    std::vector<WidgetCallback> callbacks;

    [[nodiscard]] const WidgetStruct* findStruct(std::string_view name) const noexcept;
};

// ---- tweak (P1) ------------------------------------------------------------------------------------

enum class RegistryType
{
    Dword,
    Qword,
    String,
};

std::string_view toString(RegistryType type) noexcept;

struct Tweak
{
    std::string key;   // under HKEY_CURRENT_USER, normalized: "Software\Microsoft\...\Advanced"
    std::string value; // value name ("" = the key's default value)
    RegistryType type = RegistryType::Dword;
    std::variant<std::uint64_t, std::string> data;
    // Explorer reads it only when it starts: the user is told to restart it (we never do).
    bool needsExplorerRestart = false;

    friend bool operator==(const Tweak&, const Tweak&) = default;
};

// ---- action -------------------------------------------------------------------------------------

struct PluginActionInfo
{
    std::string name; // "focus": called as "<plugin id>:focus"
    std::string label;
    std::string description;
};

// ---- the manifest -------------------------------------------------------------------------------

struct PluginManifest
{
    std::string id;
    std::string name;
    SemVer version;
    VersionReq api;
    std::vector<std::string> authors;
    std::string license;
    std::string homepage;
    std::string description;
    std::vector<PluginKind> kinds;
    std::optional<SemVer> minApp;

    PluginPermissions permissions;
    std::string script; // package path of the Luau entry point; empty = no script
    ConfigSchema config;
    std::optional<PluginWidget> widget;
    std::optional<BarConfig> skin; // what the skin sets: its `keys` say which
    std::vector<Tweak> tweaks;
    std::vector<PluginActionInfo> actions;

    [[nodiscard]] bool hasKind(PluginKind kind) const noexcept;
    // Runs in rmw-plugin-host.
    [[nodiscard]] bool hasScript() const noexcept { return !script.empty(); }
    [[nodiscard]] const PluginActionInfo* findAction(std::string_view action) const noexcept;
};

struct PluginParseOptions
{
    SemVer appVersion = currentAppVersion(); // for min_app
    SemVer apiVersion = pluginApiVersion();  // for api
};

// Reads and validates plugin.toml text (untrusted). Like parseRice: every problem lands in
// `diagnostics` with its line, column and key; unknown keys are warnings; InvalidData when there is
// at least one error. Checks the files it names only by their path syntax (the package reader and
// the bar check that they exist). Never throws.
[[nodiscard]] Result<PluginManifest> parsePluginManifest(std::string_view text, Diagnostics& diagnostics,
                                                         const PluginParseOptions& options);

// A plugin's settings: the schema's defaults, then each layer in order (the user's settings, the
// rice's [plugins."id"], a bar item's own keys). A value of the wrong type, out of range or for an
// unknown key is left out with a warning (key "<where>.<key>"): a rice written for an older
// version of the plugin still loads. A table of key -> value.
[[nodiscard]] Value resolvePluginConfig(const ConfigSchema& schema, std::span<const Value* const> layers,
                                        Diagnostics& diagnostics, std::string_view where);

} // namespace rmw::domain
