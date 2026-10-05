#pragma once

#include "TomlReader.hpp"

#include "rmw/domain/Rice.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reads a rice's [[bar]] and a plugin's [skin] (internal to rmw::domain): the same keys, the same
// checks, so a skin is exactly "a bar the rice can start from".
namespace rmw::domain::detail
{

// "#RRGGBB" or "palette.<name>". With a rice, the name must be in its [palette] ("accent" always
// is); without one (a skin), any valid name: it is resolved against the rice using the skin.
std::optional<ColorRef> readColorRef(TableReader& t, std::string_view key, const Rice* rice);

// The keys a skin can set (a bar table's keys but "monitor" and "skin").
std::span<const std::string_view> barKeys() noexcept;

class BarTableReader
{
public:
    // `rice`: the palette colors may name and the plugins it lists; nullptr for a skin.
    BarTableReader(Diagnostics& diagnostics, const Rice* rice);

    // Only the keys present change `bar` (a skin fills what a rice's bar leaves out); their names
    // are added to `bar.keys`. A skin cannot choose monitors or another skin.
    void read(const toml::table& table, const std::string& path, BarConfig& bar, bool skin);

private:
    void readBarLength(TableReader& t, BarConfig& bar);
    void readBarMargin(TableReader& t, BarConfig& bar);
    void readBarBackground(TableReader& t, BarConfig& bar);
    void readBarZone(TableReader& t, std::string_view key, std::vector<BarItem>& items);
    bool requiresPlugin(std::string_view id) const noexcept;

    Diagnostics& m_diagnostics;
    const Rice* m_rice;
};

} // namespace rmw::domain::detail
