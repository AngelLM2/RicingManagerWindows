// A plugin's tweaks: registry keys, value names, data and the engine's field names, each checked
// against the allow and block lists (docs/12 §5). Lines of the input feed the checks in turn.

#include "rmw/domain/Tweaks.hpp"

#include "rmw/modules/Tweaks.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NOLINTNEXTLINE(misc-use-internal-linkage): libFuzzer's entry point
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto line = [&text] {
        const auto end = text.find('\n');
        const std::string_view first = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        return first;
    };
    const std::string_view key = line();
    const std::string_view name = line();
    [[maybe_unused]] const auto checkedKey = rmw::domain::validateTweakKey(key);
    [[maybe_unused]] const auto checkedName = rmw::domain::validateTweakValueName(key, name);
    [[maybe_unused]] const auto checkedData = rmw::domain::validateTweakString(text);
    [[maybe_unused]] const auto field = rmw::modules::parseTweakField(key);
    return 0;
}
