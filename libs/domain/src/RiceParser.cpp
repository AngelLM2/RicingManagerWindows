#include "rmw/domain/RiceParser.hpp"

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

constexpr std::string_view kPalettePrefix = "palette.";

constexpr std::initializer_list<std::string_view> kImageExtensions = {"jpg", "jpeg", "png", "bmp"};
constexpr std::initializer_list<std::string_view> kVideoExtensions = {"mp4", "m4v"};
constexpr std::initializer_list<std::string_view> kCursorExtensions = {"cur", "ani"};

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
        if (const auto* taskbar = t.table("taskbar"))
        {
            readTaskbar(*taskbar);
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
        const auto text = t.string(key);
        if (!text)
        {
            return std::nullopt;
        }
        if (text->starts_with(kPalettePrefix))
        {
            std::string name = text->substr(kPalettePrefix.size());
            if (!isValidPaletteName(name))
            {
                t.error(key, std::format("invalid palette name '{}'", name));
                return std::nullopt;
            }
            if (name != "accent" && m_rice.findPalette(name) == nullptr)
            {
                t.error(key, std::format("'{}' is not defined in [palette]", *text));
                return std::nullopt;
            }
            return ColorRef{std::move(name)};
        }
        auto color = Color::parse(*text);
        if (!color)
        {
            t.error(key, color.error().message() + " — or use palette.<name>");
            return std::nullopt;
        }
        return ColorRef{*color};
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
            }
        }
        t.reportUnknownKeys();
        m_rice.wallpaper = std::move(config);
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
        TableReader t(table, path, m_diagnostics);
        BarConfig bar;
        if (auto monitor = t.parsed("monitor", MonitorSelector::parse))
        {
            bar.monitor = std::move(*monitor);
        }
        bar.position =
            t.enumeration<BarPosition>("position", kBarPositionNames).value_or(BarPosition::Bottom);
        bar.mode = t.enumeration<BarMode>("mode", kBarModeNames).value_or(BarMode::Reserve);
        bar.thickness = readUnsigned<std::uint32_t>(t, "thickness", 16, 512).value_or(bar.thickness);
        readBarLength(t, bar);
        readBarMargin(t, bar);
        bar.radius = readUnsigned<std::uint32_t>(t, "radius", 0, 256).value_or(0);
        readBarBackground(t, bar);
        bar.foreground = readColorRef(t, "foreground");
        bar.autohide = t.boolean("autohide").value_or(false);
        bar.hideNativeTaskbar = t.boolean("hide_native_taskbar").value_or(false);
        readBarZone(t, "left", bar.left);
        readBarZone(t, "center", bar.center);
        readBarZone(t, "right", bar.right);
        t.reportUnknownKeys();
        m_rice.bars.push_back(std::move(bar));
    }

    void readBarLength(TableReader& t, BarConfig& bar)
    {
        const toml::node* node = t.get("length");
        if (node == nullptr)
        {
            return;
        }
        if (const auto* pixels = node->as_integer())
        {
            if (pixels->get() < 16 || pixels->get() > 16'384)
            {
                t.error("length", std::format("{} px is out of range (16..16384)", pixels->get()));
                return;
            }
            bar.length = {false, static_cast<double>(pixels->get())};
            return;
        }
        if (const auto* text = node->as_string())
        {
            const std::string& value = text->get();
            double percent = 0.0;
            if (value.size() >= 2 && value.back() == '%')
            {
                const char* first = value.data();
                const char* last = value.data() + value.size() - 1;
                const auto [ptr, ec] = std::from_chars(first, last, percent, std::chars_format::fixed);
                if (ec == std::errc{} && ptr == last && percent >= 1.0 && percent <= 100.0)
                {
                    bar.length = {true, percent};
                    return;
                }
            }
        }
        t.error("length", "expected a percentage like \"100%\" (1–100) or a size in pixels like 600");
    }

    void readBarMargin(TableReader& t, BarConfig& bar)
    {
        constexpr std::int64_t kMax = 1'024;
        const toml::node* node = t.get("margin");
        if (node == nullptr)
        {
            return;
        }
        const auto checked = [&](const toml::node& item, std::uint32_t& out) {
            const auto* value = item.as_integer();
            if (value == nullptr || value->get() < 0 || value->get() > kMax)
            {
                t.error("margin", std::format("margins are integers from 0 to {}", kMax));
                return false;
            }
            out = static_cast<std::uint32_t>(value->get());
            return true;
        };

        std::array<std::uint32_t, 4> values{};
        if (node->is_integer())
        {
            if (checked(*node, values[0]))
            {
                std::ranges::fill(bar.margin, values[0]);
            }
            return;
        }
        const auto* array = node->as_array();
        if (array == nullptr || (array->size() != 2 && array->size() != 4))
        {
            t.error("margin", "expected one number, [vertical, horizontal] or [top, right, bottom, left]");
            return;
        }
        for (std::size_t i = 0; i < array->size(); ++i)
        {
            if (!checked(*array->get(i), values[i]))
            {
                return;
            }
        }
        if (array->size() == 2)
        {
            bar.margin[0] = bar.margin[2] = values[0];
            bar.margin[1] = bar.margin[3] = values[1];
        }
        else
        {
            bar.margin = values;
        }
    }

    void readBarBackground(TableReader& t, BarConfig& bar)
    {
        const toml::node* node = t.get("background");
        if (node == nullptr)
        {
            return;
        }
        if (node->is_string())
        {
            // Shorthand: background = "acrylic"
            if (const auto kind = t.enumeration<BarBackground::Kind>("background", kBarBackgroundNames))
            {
                bar.background.kind = *kind;
            }
            return;
        }
        const auto* table = node->as_table();
        if (table == nullptr)
        {
            t.error("background", std::format("expected a kind ({}) or a table {{ kind, tint, opacity }}",
                                              enumChoices<BarBackground::Kind>(kBarBackgroundNames)));
            return;
        }
        TableReader b(*table, t.pathOf("background"), m_diagnostics);
        bar.background.kind = b.enumeration<BarBackground::Kind>("kind", kBarBackgroundNames)
                                  .value_or(BarBackground::Kind::Solid);
        bar.background.tint = readColorRef(b, "tint");
        bar.background.opacity = b.number("opacity", 0.0, 1.0);
        b.reportUnknownKeys();
    }

    void readBarZone(TableReader& t, std::string_view key, std::vector<BarItem>& items)
    {
        const auto* array = t.array(key, limits::kBarMaxItemsPerZone);
        if (array == nullptr)
        {
            return;
        }
        std::size_t index = 0;
        for (const auto& node : *array)
        {
            const std::string path = indexPath(t.pathOf(key), index++);
            BarItem item;
            if (const auto* id = node.as_string())
            {
                item.id = id->get();
            }
            else if (const auto* table = node.as_table())
            {
                // { id = "clock", format = "%H:%M" }: everything but the id is the widget's config.
                const toml::node* idNode = table->get("id");
                if (idNode == nullptr || !idNode->is_string())
                {
                    errorAt(m_diagnostics, node, path, "a widget table needs id = \"<widget>\"");
                    continue;
                }
                item.id = idNode->as_string()->get();
                Value::Table config;
                std::size_t budget = limits::kConfigValueMaxElements;
                bool ok = true;
                for (const auto& [childKey, child] : *table)
                {
                    if (childKey.str() == "id")
                    {
                        continue;
                    }
                    auto value = toValue(child, childPath(path, childKey.str()), m_diagnostics, budget);
                    if (!value)
                    {
                        ok = false;
                        break;
                    }
                    config.push_back({std::string(childKey.str()), std::move(*value)});
                }
                if (!ok)
                {
                    continue;
                }
                item.config.data = std::move(config);
            }
            else
            {
                errorAt(m_diagnostics, node, path,
                        std::format("expected a widget name or {{ id = ... }}, found {}", typeName(node)));
                continue;
            }

            if (item.isPlugin() ? !isValidPackageId(item.id) : !isValidBuiltinName(item.id))
            {
                errorAt(m_diagnostics, node, path, std::format("invalid widget name '{}'", item.id));
                continue;
            }
            if (item.isPlugin() && !requiresPlugin(item.id))
            {
                warningAt(
                    m_diagnostics, node, path,
                    std::format("plugin '{}' is not listed in [plugins] requires; users will not be asked to "
                                "install it",
                                item.id));
            }
            items.push_back(std::move(item));
        }
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
