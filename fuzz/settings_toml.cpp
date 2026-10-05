// settings.toml: any text gives settings or diagnostics, never a crash (docs/06 §4). Settings that
// read are written back (the settings app does that) and must read again to the same text.

#include "rmw/domain/Settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static constexpr std::array<std::string_view, 4> actions = {"rice.apply", "rice.undo",
                                                                "appearance.toggle", "bar.toggle"};
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    rmw::domain::Diagnostics diagnostics;
    const auto settings = rmw::domain::parseSettings(text, diagnostics, actions);
    if (!settings)
    {
        return 0;
    }
    const std::string written = rmw::domain::writeSettings(*settings);
    rmw::domain::Diagnostics again;
    const auto reread = rmw::domain::parseSettings(written, again, actions);
    if (!reread || rmw::domain::writeSettings(*reread) != written)
    {
        std::abort(); // the writer lost or changed something
    }
    return 0;
}
