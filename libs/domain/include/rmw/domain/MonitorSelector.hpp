#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::domain
{

// Which monitors a rule applies to (docs/03 §6). Portable selectors (primary, left...) are for
// shared rices; "edid:<key>" pins a rule to one physical monitor (identity key, ADR-0004).
struct MonitorSelector
{
    enum class Kind
    {
        Primary,
        All, // every monitor no earlier rule matched
        Left,
        Right,
        Top,
        Bottom,
        Edid,
    };

    Kind kind = Kind::All;
    std::string edidKey; // only for Kind::Edid

    static Result<MonitorSelector> parse(std::string_view text);
    std::string toString() const;

    friend bool operator==(const MonitorSelector&, const MonitorSelector&) = default;
};

// What the selector logic needs to know about a connected monitor. Coordinates are virtual-screen
// pixels of the monitor rectangle.
struct MonitorFacts
{
    std::string identityKey;
    bool primary = false;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// Resolves rules against monitors, first match wins per monitor: for each monitor returns the
// index of the rule that applies, or -1 (keep what it has). "All" only takes monitors that no
// other rule claimed, wherever it appears. Left/Right/Top/Bottom pick the single extreme monitor
// (ties broken by the other axis, then by the primary flag).
std::vector<int> resolveMonitorRules(std::span<const MonitorSelector> rules,
                                     std::span<const MonitorFacts> monitors);

} // namespace rmw::domain
