#include "rmw/modules/Wallpaper.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Images.hpp"
#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/modules/RegistryFields.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace rmw::modules
{
namespace
{

using engine::Json;
using ports::WallpaperPosition;

constexpr std::array<std::string_view, 1> kDependsOn = {"appearance"}; // a .theme can carry a wallpaper
constexpr DwordField kBackgroundType = {
    "backgroundType", R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers)", "BackgroundType"};
constexpr std::uint32_t kPicture = 0;
constexpr std::uint32_t kSolidColor = 1;
constexpr std::uint32_t kSlideshow = 2;
constexpr std::uint32_t kSpotlight = 3;
constexpr std::uint32_t kDefaultSlideSeconds = 30 * 60;

WallpaperPosition positionOf(domain::WallpaperFit fit) noexcept
{
    switch (fit)
    {
    case domain::WallpaperFit::Fill:
        return WallpaperPosition::Fill;
    case domain::WallpaperFit::Fit:
        return WallpaperPosition::Fit;
    case domain::WallpaperFit::Stretch:
        return WallpaperPosition::Stretch;
    case domain::WallpaperFit::Center:
        return WallpaperPosition::Center;
    case domain::WallpaperFit::Tile:
        return WallpaperPosition::Tile;
    }
    return WallpaperPosition::Fill;
}

std::string hex(std::uint32_t rgb)
{
    return std::format("#{:06X}", rgb & 0xFFFFFF);
}

std::string hex(const domain::Color& color)
{
    return std::format("#{:02X}{:02X}{:02X}", color.r, color.g, color.b);
}

std::optional<std::uint32_t> rgbOf(const Json& value)
{
    if (!value.is_string())
    {
        return std::nullopt;
    }
    const auto color = domain::Color::parse(value.get_ref<const std::string&>());
    if (!color)
    {
        return std::nullopt;
    }
    return (std::uint32_t{color->r} << 16) | (std::uint32_t{color->g} << 8) | color->b;
}

std::string utf8Path(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::string lowerAscii(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

std::string monitorField(const ports::MonitorInfo& monitor)
{
    return std::string(kMonitorFieldPrefix) + monitor.identityKey;
}

bool isImageFile(std::string_view name)
{
    return domain::hasExtension(name, {"jpg", "jpeg", "png", "bmp"});
}

} // namespace

std::span<const std::string_view> WallpaperLogic::dependsOn() const noexcept
{
    return kDependsOn;
}

Result<Json> WallpaperLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const
{
    Json desired = Json::object();
    if (!rice.wallpaper)
    {
        return desired;
    }
    const domain::WallpaperConfig& config = *rice.wallpaper;
    if (config.background)
    {
        desired["background"] = hex(*config.background);
    }

    auto monitors = m_topology.monitors();
    if (!monitors)
    {
        return std::unexpected(std::move(monitors.error()).withContext("list monitors"));
    }

    if (config.layout == domain::WallpaperLayout::Span)
    {
        auto stored = assets.fromPackage(config.spanSource);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        for (const auto& monitor : *monitors)
        {
            desired[monitorField(monitor)] = Json{{"file", utf8Path(*stored)}};
        }
        desired["position"] = static_cast<std::uint32_t>(WallpaperPosition::Span);
        desired["slideshow"] = nullptr;
        desired["backgroundType"] = kPicture;
        return desired;
    }
    if (config.monitors.empty())
    {
        return desired; // background color only
    }

    // Which rule each monitor gets (first match; "all" takes the rest).
    std::vector<domain::MonitorSelector> selectors;
    selectors.reserve(config.monitors.size());
    for (const auto& rule : config.monitors)
    {
        selectors.push_back(rule.match);
    }
    std::vector<domain::MonitorFacts> facts;
    facts.reserve(monitors->size());
    for (const auto& monitor : *monitors)
    {
        facts.push_back(
            {monitor.identityKey, monitor.primary, monitor.left, monitor.top, monitor.right, monitor.bottom});
    }
    const std::vector<int> owners = domain::resolveMonitorRules(selectors, facts);
    std::vector<std::pair<const ports::MonitorInfo*, const domain::MonitorWallpaper*>> matched;
    for (std::size_t i = 0; i < monitors->size(); ++i)
    {
        if (owners[i] >= 0)
        {
            matched.emplace_back(&(*monitors)[i], &config.monitors[static_cast<std::size_t>(owners[i])]);
        }
    }
    if (matched.empty())
    {
        return desired; // no connected monitor matches: keep what they show
    }

    const auto isKind = [](domain::WallpaperKind kind) {
        return [kind](const auto& entry) { return entry.second->kind == kind; };
    };

    // Native slideshow: one playlist for every monitor.
    if (std::ranges::any_of(matched, isKind(domain::WallpaperKind::Slideshow)))
    {
        const domain::MonitorWallpaper& first = *matched.front().second;
        const bool oneSlideshow = std::ranges::all_of(matched, [&](const auto& entry) {
            return entry.second->kind == domain::WallpaperKind::Slideshow &&
                   entry.second->source == first.source;
        });
        if (!oneSlideshow)
        {
            return fail(ErrorCode::Unsupported,
                        "a slideshow on some monitors and something else on others needs the scheduler "
                        "(coming in a later version); use one slideshow for every monitor for now");
        }
        auto names = assets.listPackageFolder(first.source);
        if (!names)
        {
            return std::unexpected(std::move(names.error()));
        }
        Json items = Json::array();
        for (const auto& name : *names)
        {
            if (!isImageFile(name))
            {
                continue;
            }
            auto stored = assets.fromPackage(first.source + "/" + name);
            if (!stored)
            {
                return std::unexpected(std::move(stored.error()));
            }
            items.push_back(utf8Path(*stored));
        }
        if (items.empty())
        {
            return fail(ErrorCode::InvalidData,
                        std::format("slideshow folder '{}' has no images", first.source));
        }
        desired["slideshow"] =
            Json{{"items", std::move(items)},
                 {"shuffle", first.shuffle.value_or(false)},
                 {"intervalMs", first.intervalSeconds.value_or(kDefaultSlideSeconds) * 1000}};
        desired["position"] = static_cast<std::uint32_t>(positionOf(first.fit));
        desired["backgroundType"] = kSlideshow;
        return desired;
    }

    // Only one solid color everywhere: no image at all, just the background.
    if (std::ranges::all_of(matched, isKind(domain::WallpaperKind::Color)) &&
        std::ranges::all_of(
            matched, [&](const auto& entry) { return entry.second->color == matched.front().second->color; }))
    {
        desired["background"] = hex(*matched.front().second->color);
        for (const auto& [monitor, rule] : matched)
        {
            desired[monitorField(*monitor)] = Json{{"file", ""}};
        }
        desired["slideshow"] = nullptr;
        desired["backgroundType"] = kSolidColor;
        return desired;
    }

    // Images (and colors as small solid images). The first image rule's fit applies to all.
    domain::WallpaperFit fit = domain::WallpaperFit::Fill;
    if (const auto firstImage = std::ranges::find_if(
            matched, [](const auto& entry) { return entry.second->kind != domain::WallpaperKind::Color; });
        firstImage != matched.end())
    {
        fit = firstImage->second->fit;
    }
    std::map<std::string, std::string> storedBySource;
    for (const auto& [monitor, rule] : matched)
    {
        std::string file;
        if (rule->kind == domain::WallpaperKind::Color)
        {
            const auto size = fit == domain::WallpaperFit::Center
                                  ? domain::Size{static_cast<std::uint32_t>(monitor->width()),
                                                 static_cast<std::uint32_t>(monitor->height())}
                                  : domain::reducedAspect(static_cast<std::uint32_t>(monitor->width()),
                                                          static_cast<std::uint32_t>(monitor->height()));
            auto bmp = domain::makeSolidBmp(*rule->color, size.width, size.height);
            if (!bmp)
            {
                return std::unexpected(std::move(bmp.error()));
            }
            auto stored = assets.fromBytes(*bmp, "bmp");
            if (!stored)
            {
                return std::unexpected(std::move(stored.error()));
            }
            file = utf8Path(*stored);
        }
        else
        {
            // Video plays in a later version; its fallback image stands in until then.
            const std::string& source =
                rule->kind == domain::WallpaperKind::Video ? rule->fallbackImage : rule->source;
            if (source.empty())
            {
                log::warn("wallpaper: monitor {} has a video without fallback_image; left as it is",
                          monitor->displayName);
                continue;
            }
            if (const auto known = storedBySource.find(source); known != storedBySource.end())
            {
                file = known->second;
            }
            else
            {
                auto stored = assets.fromPackage(source);
                if (!stored)
                {
                    return std::unexpected(std::move(stored.error()));
                }
                file = utf8Path(*stored);
                storedBySource.emplace(source, file);
            }
        }
        desired[monitorField(*monitor)] = Json{{"file", file}};
    }
    desired["position"] = static_cast<std::uint32_t>(positionOf(fit));
    desired["slideshow"] = nullptr;
    desired["backgroundType"] = kPicture;
    return desired;
}

Result<void> WallpaperLogic::internalize(Json& state, engine::IAssetSink& assets) const
{
    for (auto& [field, value] : state.items())
    {
        if (!field.starts_with(kMonitorFieldPrefix) || !value.is_object() || !value.contains("file") ||
            !value["file"].is_string())
        {
            continue;
        }
        const std::string file = value["file"].get<std::string>();
        const bool keep = json::boolOr(value, "keep", false);
        const std::string extension = json::stringOr(value, "ext", "");
        value.erase("keep");
        value.erase("ext");
        if (file.empty() || keep)
        {
            continue;
        }
        auto wide = utf8To16(file);
        if (!wide)
        {
            continue;
        }
        auto stored = assets.fromSystem(std::filesystem::path(*wide), extension);
        if (!stored)
        {
            log::warn("wallpaper: cannot keep a copy of {}: {}", file, stored.error().message());
            continue;
        }
        value["file"] = utf8Path(*stored);
    }
    return {};
}

bool WallpaperLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    if (field.starts_with(kMonitorFieldPrefix) && a.is_object() && b.is_object() && a.contains("file") &&
        b.contains("file") && a["file"].is_string() && b["file"].is_string())
    {
        // Windows may hand back the path in a different case.
        return lowerAscii(a["file"].get<std::string>()) == lowerAscii(b["file"].get<std::string>());
    }
    if (field == "background")
    {
        return rgbOf(a) == rgbOf(b);
    }
    return a == b;
}

Result<std::vector<WallpaperSystem::Slot>> WallpaperSystem::slots()
{
    auto monitors = m_topology.monitors();
    if (!monitors)
    {
        return std::unexpected(std::move(monitors.error()));
    }
    auto named = m_wallpaper.monitors();
    if (!named)
    {
        return std::unexpected(std::move(named.error()));
    }
    std::vector<Slot> slots;
    for (auto& monitor : *monitors)
    {
        auto match = std::ranges::find_if(*named, [&](const ports::WallpaperMonitor& candidate) {
            return candidate.left == monitor.left && candidate.top == monitor.top &&
                   candidate.right == monitor.right && candidate.bottom == monitor.bottom;
        });
        if (match == named->end() && !monitor.devicePath.empty())
        {
            match = std::ranges::find_if(*named, [&](const ports::WallpaperMonitor& candidate) {
                return lowerAscii(candidate.devicePath) == lowerAscii(monitor.devicePath);
            });
        }
        if (match == named->end())
        {
            log::warn("wallpaper: no wallpaper slot found for monitor {}", monitor.displayName);
            continue;
        }
        slots.push_back({std::move(monitor), match->devicePath});
    }
    return slots;
}

Result<engine::CaptureResult> WallpaperSystem::capture()
{
    auto connected = slots();
    if (!connected)
    {
        return std::unexpected(std::move(connected.error()));
    }
    engine::CaptureResult result;
    for (const auto& [monitor, wallpaperPath] : *connected)
    {
        auto file = m_wallpaper.wallpaper(wallpaperPath);
        if (!file)
        {
            return std::unexpected(std::move(file.error()));
        }
        Json field = Json{{"file", *file}};
        if (!file->empty())
        {
            auto type = m_wallpaper.sniffImageType(*file);
            if (type && type->empty())
            {
                // The original file is gone: Windows' own copy is what the screen shows.
                auto transcoded = m_wallpaper.transcodedWallpaper();
                auto transcodedType =
                    transcoded ? m_wallpaper.sniffImageType(*transcoded) : Result<std::string>();
                if (transcoded && transcodedType && !transcodedType->empty())
                {
                    field = Json{{"file", *transcoded}, {"ext", *transcodedType}};
                }
            }
            else if (m_wallpaper.isWindowsFile(*file))
            {
                field["keep"] = true;
            }
        }
        result.state[monitorField(monitor)] = std::move(field);
    }

    auto position = m_wallpaper.position();
    auto background = m_wallpaper.backgroundColor();
    auto slideshow = m_wallpaper.slideshow();
    auto backgroundType = readDwordField(m_registry, kBackgroundType);
    if (!position || !background || !slideshow || !backgroundType)
    {
        return fail(ErrorCode::System, "cannot read the wallpaper settings");
    }
    result.state["position"] = static_cast<std::uint32_t>(*position);
    result.state["background"] = hex(*background);
    result.state["slideshow"] = slideshow->active ? Json{{"items", slideshow->items},
                                                         {"shuffle", slideshow->shuffle},
                                                         {"intervalMs", slideshow->intervalMs}}
                                                  : Json(nullptr);
    result.state["backgroundType"] = *backgroundType;
    if (backgroundType->is_number_integer() && backgroundType->get<std::uint32_t>() == kSpotlight)
    {
        result.fidelity = engine::Fidelity::Lossy;
        result.lossyReason =
            "Windows Spotlight cannot be restored exactly; Windows may take a while (and need "
            "the network) to show it again";
    }
    return result;
}

Result<void> WallpaperSystem::apply(const Json& desired)
{
    const auto slideshow = desired.find("slideshow");
    if (slideshow != desired.end() && slideshow->is_object())
    {
        ports::SlideshowState state;
        state.active = true;
        const auto items = slideshow->find("items");
        if (items == slideshow->end() || !items->is_array())
        {
            return fail(ErrorCode::InvalidArgument, "field 'slideshow' needs items");
        }
        for (const auto& item : *items)
        {
            if (!item.is_string())
            {
                return fail(ErrorCode::InvalidArgument, "slideshow items must be paths");
            }
            state.items.push_back(item.get<std::string>());
        }
        state.shuffle = json::boolOr(*slideshow, "shuffle", false);
        const std::int64_t interval =
            json::intOr(*slideshow, "intervalMs", std::int64_t{kDefaultSlideSeconds} * 1000);
        if (interval < 1000 || interval > 7LL * 24 * 3600 * 1000)
        {
            return fail(ErrorCode::InvalidArgument, "slideshow interval out of range");
        }
        state.intervalMs = static_cast<std::uint32_t>(interval);
        if (auto set = m_wallpaper.setSlideshow(state); !set)
        {
            return set;
        }
    }
    else
    {
        bool anyMonitor = false;
        for (const auto& [field, value] : desired.items())
        {
            anyMonitor = anyMonitor || field.starts_with(kMonitorFieldPrefix);
        }
        if (anyMonitor)
        {
            auto connected = slots();
            if (!connected)
            {
                return std::unexpected(std::move(connected.error()));
            }
            for (const auto& [field, value] : desired.items())
            {
                if (!field.starts_with(kMonitorFieldPrefix))
                {
                    continue;
                }
                if (!value.is_object() || !value.contains("file") || !value["file"].is_string())
                {
                    return fail(ErrorCode::InvalidArgument, std::format("field '{}' needs a file", field));
                }
                const std::string_view key = std::string_view(field).substr(kMonitorFieldPrefix.size());
                const auto slot = std::ranges::find_if(
                    *connected, [&](const Slot& s) { return s.monitor.identityKey == key; });
                if (slot == connected->end())
                {
                    log::warn("wallpaper: monitor {} is not connected; skipped", key);
                    continue;
                }
                if (auto set =
                        m_wallpaper.setWallpaper(slot->wallpaperPath, value["file"].get<std::string>());
                    !set)
                {
                    return set;
                }
            }
        }
    }

    if (const auto position = desired.find("position"); position != desired.end())
    {
        // Integers parsed from text are unsigned in nlohmann, literals are signed: accept both.
        if (!position->is_number_integer() || position->get<std::int64_t>() < 0 ||
            position->get<std::int64_t>() > 5)
        {
            return fail(ErrorCode::InvalidArgument, "field 'position' is out of range");
        }
        if (auto set =
                m_wallpaper.setPosition(static_cast<WallpaperPosition>(position->get<std::uint32_t>()));
            !set)
        {
            return set;
        }
    }
    if (const auto background = desired.find("background"); background != desired.end())
    {
        const auto rgb = rgbOf(*background);
        if (!rgb)
        {
            return fail(ErrorCode::InvalidArgument, "field 'background' needs \"#RRGGBB\"");
        }
        if (auto set = m_wallpaper.setBackgroundColor(*rgb); !set)
        {
            return set;
        }
    }
    if (const auto type = desired.find("backgroundType"); type != desired.end())
    {
        return writeDwordField(m_registry, kBackgroundType, *type);
    }
    return {};
}

} // namespace rmw::modules
