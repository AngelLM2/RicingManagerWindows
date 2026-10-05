#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/json/Json.hpp"
#include "rmw/ports/Display.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// "My Windows as a rice" (rmw export): turns captured module states into rice.toml text plus the
// files to copy into the package. Pure: the core captures, copies and writes.
namespace rmw::modules
{

struct ExportedFile
{
    std::string source;   // as Windows has it (may contain %VARS%)
    std::string relative; // inside the package, e.g. "assets/cursors/normal.cur"
};

struct ExportResult
{
    std::string toml;
    std::vector<ExportedFile> files;
    std::vector<std::string> notes; // what could not be expressed exactly
};

struct CapturedLook
{
    json::Json appearance; // null when not captured
    json::Json taskbar;
    json::Json cursors;
    json::Json wallpaper;
    std::vector<ports::MonitorInfo> monitors;
};

// `extensionOf(source)` gives the file type of a source (by content when needed, e.g. for
// TranscodedWallpaper); empty means "not an image we can export".
[[nodiscard]] Result<ExportResult> exportRice(
    std::string_view name, const CapturedLook& look,
    const std::function<std::string(std::string_view)>& extensionOf);

// A TOML basic string with every escape TOML requires.
[[nodiscard]] std::string tomlString(std::string_view text);

} // namespace rmw::modules
