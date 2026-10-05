// settings.toml: any text gives settings or diagnostics, never a crash (docs/06 §4).

#include "rmw/domain/Settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static constexpr std::array<std::string_view, 4> actions = {"rice.apply", "rice.undo",
                                                                "appearance.toggle", "bar.toggle"};
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    rmw::domain::Diagnostics diagnostics;
    [[maybe_unused]] const auto settings = rmw::domain::parseSettings(text, diagnostics, actions);
    return 0;
}
