#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The .slint source of a plugin widget, made safe to compile inside rmw-bar (ADR-0008: the Slint
// interpreter has no hook on what it loads, so the source is checked before it is compiled).
//
// Our own tokenizer reads the file and writes it back token by token, without comments, so what the
// compiler sees is exactly what was checked:
// - no `import` (fonts and files) except names from "std-widgets.slint", and no re-exports;
// - @image-url only with a plain relative path inside the plugin's folder, rewritten to the absolute
//   path the caller resolved (never a drive, a share or "..");
// - no Timer (the host paces updates), no windows or popups, no endless animations, no huge
//   repeaters, nothing named like the bar's own components (Rmw*);
// - every top-level name gets the plugin's prefix, so plugins never collide with each other or with
//   the bar;
// - limits on size, tokens, nesting, strings and images.
// Pure and fuzzable (docs/06 §4).
namespace rmw::bar
{

inline constexpr std::size_t kWidgetSourceMaxBytes = 64u << 10;

struct WidgetSourceOptions
{
    // Prepended to every top-level name: "Rmwp3" makes "NowPlaying" "Rmwp3-NowPlaying". Must
    // start with "Rmw" (plugins cannot write such names, so no plugin can reach another's).
    std::string prefix;
    // The components the manifest names (vertical: may be empty).
    std::string component;
    std::string verticalComponent;
    // An @image-url path, already checked to be a package path with an image extension: the
    // absolute path to put in its place (forward slashes), or an error when it is not a file of
    // the plugin. Called once per @image-url.
    std::function<Result<std::string>(std::string_view packagePath)> resolveImage;
};

struct WidgetInterface
{
    std::vector<std::string> inProperties; // `in` and `in-out` properties, '_' written as '-'
    std::vector<std::string> callbacks;
};

struct CheckedWidgetSource
{
    std::string source;                  // ready to concatenate with the other widgets
    std::vector<std::string> stdWidgets; // names imported from "std-widgets.slint"
    std::string component;               // renamed
    std::string verticalComponent;       // renamed (empty when none)
    WidgetInterface interface;           // of `component`
    WidgetInterface verticalInterface;   // of `verticalComponent`
    // A top-level name of the plugin as it is now ("Entry" -> "Rmwp3-Entry"); the name itself when
    // the plugin does not declare it.
    [[nodiscard]] std::string renamed(std::string_view name) const;

    std::vector<std::pair<std::string, std::string>> names; // declared -> renamed
};

// `fileName` only labels the errors ("widget.slint:12: ...").
[[nodiscard]] Result<CheckedWidgetSource> checkWidgetSource(std::string_view text, std::string_view fileName,
                                                            const WidgetSourceOptions& options);

} // namespace rmw::bar
