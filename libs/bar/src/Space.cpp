#include "rmw/bar/Space.hpp"

#include "rmw/json/Json.hpp"

#include <algorithm>
#include <optional>

namespace rmw::bar
{
namespace
{

using json::Json;

constexpr std::int64_t kVersion = 1;
constexpr std::size_t kMaxEntries = 64; // a bar per monitor, a few per monitor at most
constexpr std::size_t kMaxBytes = std::size_t{64} << 10;
constexpr std::int64_t kMaxCoordinate = std::int64_t{1} << 20;

Json rectJson(const Rect& rect)
{
    return Json::array({rect.left, rect.top, rect.right, rect.bottom});
}

std::optional<Rect> rectFrom(const Json& entry, std::string_view key)
{
    const auto found = entry.find(key);
    if (found == entry.end() || !found->is_array() || found->size() != 4 ||
        !std::ranges::all_of(*found, [](const Json& v) {
            return v.is_number_integer() && v.get<std::int64_t>() >= -kMaxCoordinate &&
                   v.get<std::int64_t>() <= kMaxCoordinate;
        }))
    {
        return std::nullopt;
    }
    return Rect{(*found)[0].get<std::int32_t>(), (*found)[1].get<std::int32_t>(),
                (*found)[2].get<std::int32_t>(), (*found)[3].get<std::int32_t>()};
}

} // namespace

std::string spaceToJson(std::span<const SpaceEntry> entries)
{
    Json list = Json::array();
    for (const auto& entry : entries)
    {
        list.push_back({{"window", entry.window},
                        {"monitor", rectJson(entry.monitor)},
                        {"reserved", rectJson(entry.reserved)},
                        {"claimed", entry.claimed}});
    }
    return json::dump({{"version", kVersion}, {"bars", std::move(list)}});
}

Result<std::vector<SpaceEntry>> parseSpace(std::string_view text)
{
    auto parsed = json::parse(text, json::ParseLimits{.maxBytes = kMaxBytes});
    if (!parsed)
    {
        return std::unexpected(std::move(parsed.error()));
    }
    const Json& root = *parsed;
    const auto bars = root.is_object() ? root.find("bars") : root.end();
    if (!root.is_object() || json::intOr(root, "version", 0) != kVersion || bars == root.end() ||
        !bars->is_array() || bars->size() > kMaxEntries)
    {
        return fail(ErrorCode::InvalidData, "bar space: not a version 1 list");
    }
    std::vector<SpaceEntry> entries;
    entries.reserve(bars->size());
    for (const auto& entry : *bars)
    {
        if (!entry.is_object())
        {
            return fail(ErrorCode::InvalidData, "bar space: an entry is malformed");
        }
        const auto window = entry.find("window");
        const auto claimed = entry.find("claimed");
        const auto monitor = rectFrom(entry, "monitor");
        const auto reserved = rectFrom(entry, "reserved");
        if (window == entry.end() || !window->is_number_unsigned() || window->get<std::uint64_t>() == 0 ||
            window->get<std::uint64_t>() > (std::uint64_t{1} << 48) || claimed == entry.end() ||
            !claimed->is_boolean() || !monitor || !reserved)
        {
            return fail(ErrorCode::InvalidData, "bar space: an entry is malformed");
        }
        entries.push_back({window->get<std::uint64_t>(), *monitor, *reserved, claimed->get<bool>()});
    }
    return entries;
}

Rect workAreaWithout(const SpaceEntry& entry, const Rect& work) noexcept
{
    if (entry.reserved.empty())
    {
        return work;
    }
    // Only what lies on the monitor: a strip from a monitor laid out otherwise since must not widen it.
    const Rect& m = entry.monitor;
    const Rect strip{std::max(entry.reserved.left, m.left), std::max(entry.reserved.top, m.top),
                     std::min(entry.reserved.right, m.right), std::min(entry.reserved.bottom, m.bottom)};
    if (strip.empty())
    {
        return work;
    }
    return {std::min(work.left, strip.left), std::min(work.top, strip.top), std::max(work.right, strip.right),
            std::max(work.bottom, strip.bottom)};
}

} // namespace rmw::bar
