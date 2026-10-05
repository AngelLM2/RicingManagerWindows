// rice.toml: any text gives a rice or diagnostics, never a crash (docs/06 §4). A rice that reads
// is written back (the settings app does that) and must read again to the same text.

#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(text, diagnostics);
    if (!rice)
    {
        return 0;
    }
    const std::string written = rmw::domain::writeRice(*rice);
    rmw::domain::Diagnostics again;
    const auto reread = rmw::domain::parseRice(written, again);
    if (!reread || rmw::domain::writeRice(*reread) != written)
    {
        std::abort(); // the writer lost or changed something
    }
    return 0;
}
