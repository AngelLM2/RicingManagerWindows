#include "rmw/domain/RiceParser.hpp"

#include "BarReader.hpp"
#include "EnumNames.hpp"
#include "TomlReader.hpp"

#include "rmw/base/Version.hpp"
#include "rmw/domain/Identifiers.hpp"
#include "rmw/domain/PathRules.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <initializer_list>

namespace rmw::domain
{

using namespace detail;

namespace
{

constexpr std::initializer_list<std::string_view> kImageExtensions = {"jpg", "jpeg", "png", "bmp"};
constexpr std::initializer_list<std::string_view> kVideoExtensions = {"mp4", "m4v", "webm"};
constexpr std::initializer_list<std::string_view> kCursorExtensions = {"cur", "ani"};
constexpr std::initializer_list<std::string_view> kSoundExtensions = {"wav"};
constexpr std::initializer_list<std::string_view> kIconExtensions = {"ico"};
constexpr std::initializer_list<std::string_view> kLockScreenExtensions = {"jpg", "jpeg", "png"};
constexpr std::size_t kMaxIconApps = limits::kRiceMaxIconApps;

std::string extensionList(std::initializer_list<std::string_view> extensions)
{
    std::string text;
    for (const auto extension : extensions)
    {
        text += text.empty() ? "." : ", .";
        text += extension;
    }
    return text;
}

class RiceReader
{
public:
    RiceReader(Diagnostics& diagnostics, const RiceParseOptions& options, Rice& rice)
        : m_diagnostics(diagnostics)
        , m_options(options)
        , m_rice(rice)
    {
    }

    void read(const toml::table& root)
    {
        TableReader t(root, {}, m_diagnostics);
        readSchema(t);
        if (const auto* meta = t.table("meta"))
        {
            readMeta(*meta);
        }
        else if (!t.raw().contains("meta"))
        {
            t.missing("meta");
        }
        if (const auto* palette = t.table("palette"))
        {
            readPalette(*palette);
        }
        if (const auto* appearance = t.table("appearance"))
        {
            readAppearance(*appearance);
        }
        if (const auto* wallpaper = t.table("wallpaper"))
        {
            readWallpaper(*wallpaper);
        }
        if (const auto* cursors = t.table("cursors"))
        {
            readCursors(*cursors);
        }
        if (const auto* sounds = t.table("sounds"))
        {
            readSounds(*sounds);
        }
        if (const auto* icons = t.table("icons"))
        {
            readIcons(*icons);
        }
        if (const auto* lockscreen = t.table("lockscreen"))
        {
            readLockScreen(*lockscreen);
        }
        if (const auto* windows = t.table("windows"))
        {
            readWindows(*windows);
        }
        if (const auto* taskbar = t.table("taskbar"))
        {
            readTaskbar(*taskbar);
        }
        if (const auto* start = t.table("start"))
        {
            readStart(*start);
        }
        if (const auto* plugins = t.table("plugins"))
        {
            readPlugins(*plugins); // before [[bar]], which checks widget plugins against it
        }
        readBars(t);
        t.reportUnknownKeys();
    }

private:
    // ---- shared helpers ------------------------------------------------------------------------

    // A package-relative path; `extensions` empty means a folder (or any file).
    std::optional<std::string> readPath(TableReader& t, std::string_view key,
                                        std::initializer_list<std::string_view> extensions = {})
    {
        const auto text = t.string(key, limits::kRelativePathMaxChars);
        if (!text)
        {
            return std::nullopt;
        }
        auto path = validatePackagePath(*text);
        if (!path)
        {
            t.error(key, path.error().message());
            return std::nullopt;
        }
        if (!std::empty(extensions) && !hasExtension(*path, extensions))
        {
            t.error(key, std::format("unsupported file type '{}' (expected {})", *path,
                                     extensionList(extensions)));
            return std::nullopt;
        }
        return std::move(*path);
    }

    std::optional<ColorRef> readColorRef(TableReader& t, std::string_view key)
    {
        return detail::readColorRef(t, key, &m_rice);
    }

    template <class T>
    std::optional<T> readUnsigned(TableReader& t, std::string_view key, std::int64_t min, std::int64_t max)
    {
        if (const auto value = t.integer(key, min, max))
        {
            return static_cast<T>(*value);
        }
        return std::nullopt;
    }

    // ---- sections ------------------------------------------------------------------------------

    void readSchema(TableReader& t)
    {
        const toml::node* node = t.get("schema");
        if (node == nullptr)
        {
            m_diagnostics.error(
                "schema", std::format("missing 'schema = {}' at the top of the file", kRiceSchemaVersion));
            return;
        }
        const auto* value = node->as_integer();
        if (value == nullptr)
        {
            t.error("schema", std::format("expected an integer, found {}", typeName(*node)));
            return;
        }
        if (value->get() > kRiceSchemaVersion)
        {
            t.error("schema",
                    std::format("this rice uses format {}; update Ricing Manager to read it (this version "
                                "reads format {})",
                                value->get(), kRiceSchemaVersion));
        }
        else if (value->get() < kRiceSchemaVersion)
        {
            t.error("schema",
                    std::format("unknown format {} (expected {})", value->get(), kRiceSchemaVersion));
        }
    }

    void readMeta(const toml::table& table)
    {
        TableReader t(table, "meta", m_diagnostics);
        RiceMeta& meta = m_rice.meta;

        if (const auto id = t.string("id"))
        {
            if (isValidPackageId(*id))
            {
                meta.id = *id;
            }
            else
            {
                t.error("id", std::format("invalid id '{}' (expected reverse-DNS like io.github.you.my-rice)",
                                          *id));
            }
        }
        if (auto name = t.string("name"))
        {
            if (name->empty())
            {
                t.error("name", "name cannot be empty");
            }
            meta.name = std::move(*name);
        }
        else if (!t.raw().contains("name"))
        {
            t.missing("name");
        }
        meta.author = t.string("author").value_or("");
        meta.license = t.string("license").value_or("");
        meta.description = t.string("description", limits::kLongTextMaxBytes).value_or("");
        meta.version = t.parsed("version", SemVer::parse);
        meta.minApp = t.parsed("min_app", SemVer::parse);
        meta.shareable = t.boolean("shareable");
        if (meta.minApp && *meta.minApp > m_options.appVersion)
        {
            t.error("min_app", std::format("this rice needs Ricing Manager {} or newer (this is {})",
                                           meta.minApp->toString(), m_options.appVersion.toString()));
        }
        t.reportUnknownKeys();
    }

    void readPalette(const toml::table& table)
    {
        TableReader t(table, "palette", m_diagnostics);
        if (table.size() > limits::kPaletteMaxColors)
        {
            m_diagnostics.error("palette", std::format("too many colors ({}, limit {})", table.size(),
                                                       limits::kPaletteMaxColors));
            return;
        }
        for (const auto& [key, node] : table)
        {
            const std::string_view name = key.str();
            if (!isValidPaletteName(name))
            {
                t.get(name);
                t.error(name, "invalid palette name (use lower-case letters, digits, '-' and '_')");
                continue;
            }
            if (auto color = t.parsed(name, Color::parse))
            {
                m_rice.palette.push_back({std::string(name), *color});
            }
        }
    }

    void readAppearance(const toml::table& table)
    {
        TableReader t(table, "appearance", m_diagnostics);
        AppearanceConfig config;
        config.mode = t.enumeration<ThemeMode>("mode", kThemeModeNames);
        config.accent = t.parsed("accent", Color::parse);
        config.transparency = t.boolean("transparency");
        config.accentOnTaskbar = t.boolean("accent_on_taskbar");

        if (const auto* schedule = t.table("schedule"))
        {
            TableReader s(*schedule, t.pathOf("schedule"), m_diagnostics);
            AppearanceSchedule times;
            times.lightAt = s.parsed("light_at", TimeOfDay::parse).value_or(times.lightAt);
            times.darkAt = s.parsed("dark_at", TimeOfDay::parse).value_or(times.darkAt);
            if (times.lightAt == times.darkAt)
            {
                s.error("dark_at", "light_at and dark_at must be different times");
            }
            s.reportUnknownKeys();
            config.schedule = times;
            if (config.mode != ThemeMode::Schedule)
            {
                t.warning("schedule", "only used when mode = \"schedule\" (ignored)");
            }
        }
        else if (config.mode == ThemeMode::Schedule)
        {
            config.schedule = AppearanceSchedule{}; // documented defaults: 07:00 / 19:00
        }
        t.reportUnknownKeys();
        m_rice.appearance = config;
    }

    void readWallpaper(const toml::table& table)
    {
        TableReader t(table, "wallpaper", m_diagnostics);
        WallpaperConfig config;
        config.layout = t.enumeration<WallpaperLayout>("mode", kWallpaperLayoutNames)
                            .value_or(WallpaperLayout::PerMonitor);
        config.background = t.parsed("background", Color::parse);

        if (const auto* policy = t.table("policy"))
        {
            TableReader p(*policy, t.pathOf("policy"), m_diagnostics);
            config.policy.pauseOnFullscreen = p.boolean("pause_on_fullscreen");
            config.policy.pauseOnBattery = p.boolean("pause_on_battery");
            config.policy.pauseWhenCovered = p.boolean("pause_when_covered");
            config.policy.fpsLimit = readUnsigned<std::uint32_t>(p, "fps_limit", 1, 240);
            config.policy.mute = p.boolean("mute");
            p.reportUnknownKeys();
        }

        const bool hasRules = table.contains("monitor");
        if (config.layout == WallpaperLayout::Span)
        {
            if (auto source = readPath(t, "source", kImageExtensions))
            {
                config.spanSource = std::move(*source);
            }
            else if (!table.contains("source"))
            {
                t.missing("source");
            }
            if (hasRules)
            {
                t.get("monitor");
                t.error("monitor",
                        "mode = \"span\" uses one image for all monitors; remove [[wallpaper.monitor]] "
                        "or use mode = \"per-monitor\"");
            }
        }
        else
        {
            if (table.contains("source"))
            {
                t.get("source");
                t.error("source",
                        "'source' here is only for mode = \"span\"; put it in [[wallpaper.monitor]]");
            }
            if (const auto* rules = t.array("monitor", limits::kRiceMaxMonitorRules))
            {
                std::size_t index = 0;
                for (const auto& node : *rules)
                {
                    const std::string path = indexPath(t.pathOf("monitor"), index++);
                    if (const auto* rule = node.as_table())
                    {
                        readMonitorRule(*rule, path, config.monitors);
                    }
                    else
                    {
                        errorAt(m_diagnostics, node, path, "expected a table ([[wallpaper.monitor]])");
                    }
                }
                warnAboutUnfinishedWallpaperFeatures(t, config);
            }
        }
        t.reportUnknownKeys();
        m_rice.wallpaper = std::move(config);
    }

    // Say what this version does instead of silently differing from the rice.
    static void warnAboutUnfinishedWallpaperFeatures(TableReader& t, const WallpaperConfig& config)
    {
        std::optional<WallpaperFit> firstFit;
        for (const auto& rule : config.monitors)
        {
            if (rule.kind == WallpaperKind::Color)
            {
                continue;
            }
            if (!firstFit)
            {
                firstFit = rule.fit;
            }
            else if (rule.fit != *firstFit)
            {
                t.warning("monitor",
                          std::format("for now every monitor uses fit = \"{}\" (a different fit per "
                                      "monitor needs pre-rendering, coming in a later version)",
                                      toString(*firstFit)));
                break;
            }
        }
        if (std::ranges::any_of(config.monitors, [](const auto& r) {
                return r.kind == WallpaperKind::Video && r.fallbackImage.empty();
            }))
        {
            t.warning("monitor", "a video without fallback_image leaves that monitor's wallpaper as it was "
                                 "while the video is paused or cannot play");
        }
    }

    void readMonitorRule(const toml::table& table, const std::string& path,
                         std::vector<MonitorWallpaper>& rules)
    {
        TableReader t(table, path, m_diagnostics);
        MonitorWallpaper rule;
        if (auto match = t.parsed("match", MonitorSelector::parse))
        {
            rule.match = std::move(*match);
        }
        rule.kind = t.enumeration<WallpaperKind>("kind", kWallpaperKindNames).value_or(WallpaperKind::Image);

        const auto notFor = [&](std::string_view key) {
            if (t.get(key) != nullptr)
            {
                t.warning(key, std::format("does not apply to kind \"{}\" (ignored)", toString(rule.kind)));
            }
        };
        const auto requirePath = [&](std::initializer_list<std::string_view> extensions) {
            if (auto source = readPath(t, "source", extensions))
            {
                rule.source = std::move(*source);
            }
            else if (!table.contains("source"))
            {
                t.missing("source");
            }
        };
        const auto readFit = [&] {
            rule.fit = t.enumeration<WallpaperFit>("fit", kWallpaperFitNames).value_or(WallpaperFit::Fill);
        };

        switch (rule.kind)
        {
        case WallpaperKind::Image:
            requirePath(kImageExtensions);
            readFit();
            notFor("color");
            notFor("fallback_image");
            notFor("interval");
            notFor("shuffle");
            break;
        case WallpaperKind::Slideshow:
            requirePath({});
            readFit();
            rule.intervalSeconds = readUnsigned<std::uint32_t>(t, "interval", 10, 86'400);
            rule.shuffle = t.boolean("shuffle");
            notFor("color");
            notFor("fallback_image");
            break;
        case WallpaperKind::Video:
            requirePath(kVideoExtensions);
            readFit();
            if (rule.fit == WallpaperFit::Tile)
            {
                t.error("fit", "\"tile\" is not available for videos");
            }
            if (auto fallback = readPath(t, "fallback_image", kImageExtensions))
            {
                rule.fallbackImage = std::move(*fallback);
            }
            notFor("color");
            notFor("interval");
            notFor("shuffle");
            break;
        case WallpaperKind::Color:
            rule.color = t.parsed("color", Color::parse);
            if (!table.contains("color"))
            {
                t.missing("color");
            }
            notFor("source");
            notFor("fit");
            notFor("fallback_image");
            notFor("interval");
            notFor("shuffle");
            break;
        }

        if (rule.kind == WallpaperKind::Slideshow)
        {
            notFor("effects");
            notFor("particles");
        }
        else
        {
            if (const auto* effects = t.table("effects"))
            {
                rule.effects = readEffects(*effects, t.pathOf("effects"), rule.kind);
            }
            if (const auto* layers = t.array("particles", limits::kWallpaperMaxParticleLayers))
            {
                std::size_t index = 0;
                for (const auto& node : *layers)
                {
                    const std::string layerPath = indexPath(t.pathOf("particles"), index++);
                    if (const auto* layer = node.as_table())
                    {
                        rule.particles.push_back(readParticleLayer(*layer, layerPath));
                    }
                    else
                    {
                        errorAt(m_diagnostics, node, layerPath,
                                "expected a table ({ kind = \"sparkles\", ... })");
                    }
                }
            }
        }

        for (const auto& earlier : rules)
        {
            if (earlier.match == rule.match)
            {
                t.warning("match",
                          std::format("an earlier rule already uses match = \"{}\"; this one never applies",
                                      rule.match.toString()));
                break;
            }
        }
        t.reportUnknownKeys();
        rules.push_back(std::move(rule));
    }

    WallpaperEffects readEffects(const toml::table& table, const std::string& path, WallpaperKind kind)
    {
        TableReader t(table, path, m_diagnostics);
        WallpaperEffects effects;
        effects.brightness = t.number("brightness", -1.0, 1.0);
        effects.contrast = t.number("contrast", 0.0, 2.0);
        effects.saturation = t.number("saturation", 0.0, 2.0);
        effects.hue = t.number("hue", -180.0, 180.0);
        effects.speed = t.number("speed", 0.25, 4.0);
        effects.blur = t.number("blur", 0.0, 40.0);
        effects.parallax = t.number("parallax", 0.0, 1.0);
        effects.zoom = t.number("zoom", 0.0, 1.0);
        effects.vignette = t.number("vignette", 0.0, 1.0);
        if (effects.speed && kind != WallpaperKind::Video)
        {
            t.warning("speed", "only a video has a speed (ignored)");
            effects.speed.reset();
        }
        if (kind == WallpaperKind::Color)
        {
            // A color has nothing to blur, move or zoom; brightness and the rest are just
            // another color, but they do no harm.
            for (const std::string_view key : {"blur", "parallax", "zoom"})
            {
                if (table.contains(key))
                {
                    t.warning(key, "does not apply to kind \"color\" (ignored)");
                }
            }
            effects.blur.reset();
            effects.parallax.reset();
            effects.zoom.reset();
        }
        t.reportUnknownKeys();
        return effects;
    }

    ParticleLayer readParticleLayer(const toml::table& table, const std::string& path)
    {
        TableReader t(table, path, m_diagnostics);
        ParticleLayer layer;
        if (auto kind = t.enumeration<ParticleKind>("kind", kParticleKindNames))
        {
            layer.kind = *kind;
        }
        else if (!table.contains("kind"))
        {
            t.missing("kind");
        }
        layer.color = readColorRef(t, "color");
        layer.count = readUnsigned<std::uint32_t>(t, "count", 1, limits::kParticleMaxCount);
        layer.size = t.number("size", 0.25, 4.0);
        layer.speed = t.number("speed", 0.0, 4.0);
        layer.direction = t.number("direction", -360.0, 360.0);
        layer.glow = t.boolean("glow");
        t.reportUnknownKeys();
        return layer;
    }

    void readCursors(const toml::table& table)
    {
        TableReader t(table, "cursors", m_diagnostics);
        CursorConfig config;
        if (auto source = readPath(t, "source"))
        {
            config.source = std::move(*source);
        }
        else if (!table.contains("source"))
        {
            t.missing("source");
        }
        config.size = readUnsigned<std::uint32_t>(t, "size", 32, 256);
        if (config.size && *config.size % 16 != 0)
        {
            t.error("size", "size must be a multiple of 16 (32, 48, 64 … 256)");
        }

        if (const auto* map = t.table("map"))
        {
            TableReader m(*map, t.pathOf("map"), m_diagnostics);
            for (const auto& [key, node] : *map)
            {
                const std::string_view name = key.str();
                const auto role = cursorRoleFromRiceName(name);
                if (!role)
                {
                    m.get(name);
                    m.error(name,
                            "unknown cursor role (normal, help, working, busy, precision, text, handwriting, "
                            "unavailable, vertical, horizontal, diagonal1, diagonal2, move, alternate, link, "
                            "location, person)");
                    continue;
                }
                if (auto file = readPath(m, name, kCursorExtensions))
                {
                    config.map.push_back({*role, std::move(*file)});
                }
            }
        }
        t.reportUnknownKeys();
        m_rice.cursors = std::move(config);
    }

    void readSounds(const toml::table& table)
    {
        TableReader t(table, "sounds", m_diagnostics);
        SoundsConfig config;
        config.scheme = t.enumeration<SoundScheme>("scheme", kSoundSchemeNames);
        if (const auto* events = t.table("events"))
        {
            TableReader e(*events, t.pathOf("events"), m_diagnostics);
            for (const auto& [key, node] : *events)
            {
                const std::string_view name = key.str();
                const auto event = soundEventFromRiceName(name);
                if (!event)
                {
                    e.get(name);
                    std::string known;
                    for (const auto& entry : kSoundEventNames)
                    {
                        known += (known.empty() ? "" : ", ") + std::string(entry.riceName);
                    }
                    e.error(name, std::format("unknown sound event (known: {})", known));
                    continue;
                }
                // "" silences that event; anything else is a .wav of the package.
                if (node.is_string() && node.as_string()->get().empty())
                {
                    e.get(name);
                    config.events.push_back({*event, {}});
                }
                else if (auto file = readPath(e, name, kSoundExtensions))
                {
                    config.events.push_back({*event, std::move(*file)});
                }
            }
        }
        if (!config.scheme && config.events.empty())
        {
            t.warning("events", "nothing to change: say scheme = \"none\" or give [sounds.events]");
        }
        t.reportUnknownKeys();
        m_rice.sounds = std::move(config);
    }

    void readIcons(const toml::table& table)
    {
        TableReader t(table, "icons", m_diagnostics);
        IconsConfig config;
        for (const auto& entry : kDesktopIconNames)
        {
            if (auto file = readPath(t, entry.name, kIconExtensions))
            {
                config.desktop.emplace_back(entry.value, std::move(*file));
            }
        }
        if (const auto* folders = t.table("folders"))
        {
            TableReader f(*folders, t.pathOf("folders"), m_diagnostics);
            for (const auto& entry : kUserFolderNames)
            {
                if (auto file = readPath(f, entry.name, kIconExtensions))
                {
                    config.folders.emplace_back(entry.value, std::move(*file));
                }
            }
            f.reportUnknownKeys();
        }
        if (const auto* apps = t.table("apps"))
        {
            TableReader a(*apps, t.pathOf("apps"), m_diagnostics);
            for (const auto& [key, node] : *apps)
            {
                const std::string name(key.str());
                if (!isValidProgramName(name))
                {
                    a.get(name);
                    a.error(name, "a program's file name, as \"firefox.exe\"");
                    continue;
                }
                if (config.apps.size() >= kMaxIconApps)
                {
                    a.get(name);
                    a.error(name, std::format("at most {} programs", kMaxIconApps));
                    continue;
                }
                if (auto file = readPath(a, name, kIconExtensions))
                {
                    std::string lower = name;
                    std::ranges::transform(lower, lower.begin(), [](char c) {
                        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                    });
                    config.apps.emplace_back(std::move(lower), std::move(*file));
                }
            }
        }
        if (const auto* types = t.table("types"))
        {
            TableReader y(*types, t.pathOf("types"), m_diagnostics);
            for (const auto& [key, node] : *types)
            {
                std::string extension(key.str());
                std::ranges::transform(extension, extension.begin(), [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                });
                if (extension.starts_with('.'))
                {
                    extension.erase(0, 1);
                }
                if (!isValidFileExtension(extension))
                {
                    y.get(key.str());
                    y.error(key.str(), "a file type, as \"txt\"");
                    continue;
                }
                if (config.types.size() >= kMaxIconApps)
                {
                    y.get(key.str());
                    y.error(key.str(), std::format("at most {} file types", kMaxIconApps));
                    continue;
                }
                if (auto file = readPath(y, key.str(), kIconExtensions))
                {
                    config.types.emplace_back(std::move(extension), std::move(*file));
                }
            }
        }
        if (const auto* machine = t.table("machine"))
        {
            TableReader m(*machine, t.pathOf("machine"), m_diagnostics);
            for (const auto& entry : kMachineIconNames)
            {
                if (auto file = readPath(m, entry.name, kIconExtensions))
                {
                    config.machine.emplace_back(entry.value, std::move(*file));
                }
            }
            m.reportUnknownKeys();
        }
        if (config.desktop.empty() && config.folders.empty() && config.apps.empty() && config.types.empty() &&
            config.machine.empty())
        {
            t.warning("folders", "nothing to change: give desktop icons, [icons.folders], [icons.apps] or [icons.types]");
        }
        t.reportUnknownKeys();
        m_rice.icons = std::move(config);
    }

    void readLockScreen(const toml::table& table)
    {
        TableReader t(table, "lockscreen", m_diagnostics);
        LockScreenConfig config;
        if (auto file = readPath(t, "image", kLockScreenExtensions))
        {
            config.image = std::move(*file);
        }
        config.spotlight = t.boolean("spotlight");
        config.tips = t.boolean("tips");
        if (!config.image.empty() && config.spotlight == true)
        {
            t.error("spotlight", "Windows Spotlight shows its own pictures: give image or spotlight = true, not both");
        }
        if (config.image.empty() && !config.spotlight && !config.tips)
        {
            t.warning("image", "nothing to change: give image, spotlight or tips");
        }
        t.reportUnknownKeys();
        m_rice.lockscreen = std::move(config);
    }

    void readWindows(const toml::table& table)
    {
        TableReader t(table, "windows", m_diagnostics);
        WindowsLookConfig config;
        config.corners = t.enumeration<WindowCorners>("corners", kWindowCornersNames);
        if (const auto* border = t.raw().get("border"); border != nullptr && border->value_or(std::string_view()) == "none")
        {
            t.get("border");
            config.noBorder = true;
        }
        else
        {
            config.border = readColorRef(t, "border");
        }
        config.caption = readColorRef(t, "caption");
        config.title = readColorRef(t, "title");
        config.backdrop = t.enumeration<WindowBackdrop>("backdrop", kWindowBackdropNames);
        config.dark = t.boolean("dark");
        if (const auto* exclude = t.array("exclude", limits::kRiceMaxIconApps))
        {
            std::size_t index = 0;
            for (const auto& node : *exclude)
            {
                const std::string path = indexPath(t.pathOf("exclude"), index++);
                const auto* name = node.as_string();
                if (name == nullptr || !isValidProgramName(name->get()))
                {
                    errorAt(m_diagnostics, node, path, "expected a program's file name, as \"code.exe\"");
                    continue;
                }
                std::string lower = name->get();
                std::ranges::transform(lower, lower.begin(), [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                });
                if (std::ranges::find(config.exclude, lower) == config.exclude.end())
                {
                    config.exclude.push_back(std::move(lower));
                }
            }
        }
        if (config.empty())
        {
            t.warning("corners", "nothing to change: give corners, border, caption, title, backdrop or dark");
        }
        t.reportUnknownKeys();
        m_rice.windows = std::move(config);
    }

    void readTaskbar(const toml::table& table)
    {
        TableReader t(table, "taskbar", m_diagnostics);
        TaskbarConfig config;
        config.alignment = t.enumeration<TaskbarAlignment>("alignment", kTaskbarAlignmentNames);
        config.autohide = t.boolean("autohide");
        config.taskView = t.boolean("task_view");
        config.search = t.enumeration<TaskbarSearch>("search", kTaskbarSearchNames);
        t.reportUnknownKeys();
        m_rice.taskbar = config;
    }

    void readStart(const toml::table& table)
    {
        TableReader t(table, "start", m_diagnostics);
        StartConfig config;
        config.menu = t.enumeration<StartMenuKind>("menu", kStartMenuNames);
        config.layout = t.enumeration<StartLayout>("layout", kStartLayoutNames);
        config.recommendations = t.boolean("recommendations");
        if (const auto* pinned = t.array("pinned", limits::kStartMaxPinned))
        {
            std::size_t index = 0;
            for (const auto& node : *pinned)
            {
                const std::string path = indexPath(t.pathOf("pinned"), index++);
                const auto* id = node.as_string();
                if (id == nullptr || id->get().empty() || id->get().size() > 256)
                {
                    errorAt(m_diagnostics, node, path, "expected an app id, as `rmw apps` lists them");
                    continue;
                }
                if (std::ranges::find(config.pinned, id->get()) == config.pinned.end())
                {
                    config.pinned.push_back(id->get());
                }
            }
        }
        t.reportUnknownKeys();
        m_rice.start = std::move(config);
    }

    void readBars(TableReader& root)
    {
        const toml::node* node = root.get("bar");
        if (node == nullptr)
        {
            return;
        }
        if (node->is_table())
        {
            root.error("bar", "write [[bar]] (double brackets): a rice can have several bars");
            return;
        }
        const auto* bars = root.array("bar", limits::kRiceMaxBars);
        if (bars == nullptr)
        {
            return;
        }
        std::size_t index = 0;
        for (const auto& element : *bars)
        {
            const std::string path = indexPath("bar", index++);
            if (const auto* table = element.as_table())
            {
                readBar(*table, path);
            }
            else
            {
                errorAt(m_diagnostics, element, path, "expected a table ([[bar]])");
            }
        }
    }

    void readBar(const toml::table& table, const std::string& path)
    {
        BarConfig bar;
        BarTableReader(m_diagnostics, &m_rice).read(table, path, bar, false);
        m_rice.bars.push_back(std::move(bar));
    }

    void readPlugins(const toml::table& table)
    {
        TableReader t(table, "plugins", m_diagnostics);
        if (const auto* requirements = t.array("requires", limits::kRiceMaxPluginRequirements))
        {
            std::size_t index = 0;
            for (const auto& node : *requirements)
            {
                const std::string path = indexPath(t.pathOf("requires"), index++);
                const auto* text = node.as_string();
                if (text == nullptr)
                {
                    errorAt(m_diagnostics, node, path,
                            R"(expected "<plugin id>" or "<plugin id>@<version>")");
                    continue;
                }
                const std::string_view spec = text->get();
                const auto at = spec.find('@');
                const std::string_view id = spec.substr(0, at);
                if (!isValidPackageId(id))
                {
                    errorAt(m_diagnostics, node, path, std::format("invalid plugin id '{}'", id));
                    continue;
                }
                PluginRequirement requirement{std::string(id), {}};
                if (at != std::string_view::npos)
                {
                    auto version = VersionReq::parse(spec.substr(at + 1));
                    if (!version)
                    {
                        errorAt(m_diagnostics, node, path, version.error().message());
                        continue;
                    }
                    requirement.version = *version;
                }
                if (requiresPlugin(requirement.id))
                {
                    errorAt(m_diagnostics, node, path,
                            std::format("plugin '{}' is listed twice", requirement.id));
                    continue;
                }
                m_rice.pluginRequirements.push_back(std::move(requirement));
            }
        }

        // [plugins."io.github.someone.widget"] tables hold each plugin's settings.
        for (const auto& [key, node] : table)
        {
            const std::string_view id = key.str();
            if (id == "requires")
            {
                continue;
            }
            t.get(id);
            if (!node.is_table() || !isValidPackageId(id))
            {
                t.error(id, R"(expected [plugins."<plugin id>"] with that plugin's settings)");
                continue;
            }
            std::size_t budget = limits::kConfigValueMaxElements;
            if (auto config = toValue(node, t.pathOf(id), m_diagnostics, budget))
            {
                m_rice.pluginSettings.push_back({std::string(id), std::move(*config)});
            }
        }
        t.reportUnknownKeys();
    }

    bool requiresPlugin(std::string_view id) const noexcept
    {
        for (const auto& requirement : m_rice.pluginRequirements)
        {
            if (requirement.id == id)
            {
                return true;
            }
        }
        return false;
    }

    Diagnostics& m_diagnostics;
    const RiceParseOptions& m_options;
    Rice& m_rice;
};

} // namespace

SemVer currentAppVersion() noexcept
{
    return SemVer{kVersionMajor, kVersionMinor, kVersionPatch, {}};
}

Result<Rice> parseRice(std::string_view text, Diagnostics& diagnostics, const RiceParseOptions& options)
{
    Rice rice;
    if (auto root = parseToml(text, diagnostics))
    {
        RiceReader(diagnostics, options, rice).read(*root);
    }

    if (diagnostics.hasErrors())
    {
        std::size_t errors = 0;
        const Diagnostic* first = nullptr;
        for (const auto& item : diagnostics.all())
        {
            if (item.severity == Severity::Error)
            {
                first = first != nullptr ? first : &item;
                ++errors;
            }
        }
        return fail(ErrorCode::InvalidData,
                    std::format("rice.toml has {} error{}; first: {}", errors, errors == 1 ? "" : "s",
                                first->toString("rice.toml")));
    }
    return rice;
}

} // namespace rmw::domain
