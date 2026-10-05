// A plugin's widget.slint: our tokenizer and checks run before the bar hands it to Slint (docs/12
// §4); what passes must come out with the plugin's prefix on every name it declares.

#include "rmw/bar/WidgetSource.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    rmw::bar::WidgetSourceOptions options;
    options.prefix = "Rmwp1";
    options.component = "Widget";
    options.verticalComponent = "WidgetVertical";
    options.resolveImage = [](std::string_view path) -> rmw::Result<std::string> {
        return "C:/plugins/fuzz/" + std::string(path);
    };
    [[maybe_unused]] const auto checked = rmw::bar::checkWidgetSource(text, "widget.slint", options);
    return 0;
}
