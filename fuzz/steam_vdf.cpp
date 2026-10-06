// Steam's libraryfolders.vdf (M12: where Wallpaper Engine's wallpapers are): a file of the user's,
// read for its "path" entries.

#include "rmw/settings/WallpaperEngine.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    [[maybe_unused]] const auto libraries =
        rmw::settings::steamLibraries(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
