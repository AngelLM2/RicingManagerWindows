#include "rmw/domain/RiceWriter.hpp"

#include "BarReader.hpp"
#include "EnumNames.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace rmw::domain
{

using namespace detail;

namespace
{

bool isBareKey(std::string_view key) noexcept
{
    return !key.empty() && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_';
    });
}

std::string number(double value)
{
    if (std::isnan(value))
    {
        return "nan";
    }
    if (std::isinf(value))
    {
        return value < 0 ? "-inf" : "inf";
    }
    std::string text = std::format("{}", value); // the shortest text that reads back the same
    if (text.find_first_of(".eE") == std::string::npos)
    {
        text += ".0"; // or TOML reads an integer
    }
    return text;
}

std::string timeOfDay(TimeOfDay time)
{
    return std::format("{:02}:{:02}", time.hour, time.minute);
}

class Writer
{
public:
    std::string take() { return std::move(m_text); }

    void line(std::string_view text)
    {
        m_text += text;
        m_text += '\n';
    }
    void header(std::string_view name)
    {
        separate();
        line(std::format("[{}]", name));
    }
    void arrayHeader(std::string_view name)
    {
        separate();
        line(std::format("[[{}]]", name));
    }
    void key(std::string_view name, std::string_view value)
    {
        line(std::format("{} = {}", tomlKey(name), value));
    }
    void string(std::string_view name, std::string_view value) { key(name, tomlQuote(value)); }
    void boolean(std::string_view name, bool value) { key(name, value ? "true" : "false"); }
    void integer(std::string_view name, std::int64_t value) { key(name, std::to_string(value)); }

    template <class T>
    void optional(std::string_view name, const std::optional<T>& value)
    {
        if (!value)
        {
            return;
        }
        if constexpr (std::is_same_v<T, bool>)
        {
            boolean(name, *value);
        }
        else
        {
            integer(name, static_cast<std::int64_t>(*value));
        }
    }

private:
    void separate()
    {
        if (!m_text.empty())
        {
            m_text += '\n';
        }
    }

    std::string m_text;
};

void writeMeta(Writer& w, const RiceMeta& meta)
{
    w.header("meta");
    if (!meta.id.empty())
    {
        w.string("id", meta.id);
    }
    w.string("name", meta.name);
    if (!meta.author.empty())
    {
        w.string("author", meta.author);
    }
    if (!meta.description.empty())
    {
        w.string("description", meta.description);
    }
    if (meta.version)
    {
        w.string("version", meta.version->toString());
    }
    if (!meta.license.empty())
    {
        w.string("license", meta.license);
    }
    if (meta.minApp)
    {
        w.string("min_app", meta.minApp->toString());
    }
    w.optional("shareable", meta.shareable);
}

void writeAppearance(Writer& w, const AppearanceConfig& appearance)
{
    w.header("appearance");
    if (appearance.mode)
    {
        w.string("mode", enumToName<ThemeMode>(kThemeModeNames, *appearance.mode));
    }
    if (appearance.accent)
    {
        w.string("accent", appearance.accent->toHex());
    }
    w.optional("transparency", appearance.transparency);
    w.optional("accent_on_taskbar", appearance.accentOnTaskbar);
    if (appearance.schedule)
    {
        w.header("appearance.schedule");
        w.string("light_at", timeOfDay(appearance.schedule->lightAt));
        w.string("dark_at", timeOfDay(appearance.schedule->darkAt));
    }
}

// `{ a = 1.0, b = true }`: the fields that have a value, in order.
class InlineTable
{
public:
    void add(std::string_view name, std::string_view value)
    {
        m_text += m_text.empty() ? "{ " : ", ";
        m_text += std::format("{} = {}", tomlKey(name), value);
    }
    void add(std::string_view name, const std::optional<double>& value)
    {
        if (value)
        {
            add(name, number(*value));
        }
    }
    bool empty() const noexcept { return m_text.empty(); }
    std::string take() { return m_text.empty() ? "{}" : std::move(m_text) + " }"; }

private:
    std::string m_text;
};

void writeEffects(Writer& w, const WallpaperEffects& effects)
{
    if (effects.empty())
    {
        return;
    }
    InlineTable table;
    table.add("brightness", effects.brightness);
    table.add("contrast", effects.contrast);
    table.add("saturation", effects.saturation);
    table.add("hue", effects.hue);
    table.add("speed", effects.speed);
    table.add("blur", effects.blur);
    table.add("parallax", effects.parallax);
    table.add("zoom", effects.zoom);
    table.add("vignette", effects.vignette);
    w.key("effects", table.take());
}

void writeParticles(Writer& w, const std::vector<ParticleLayer>& particles)
{
    if (particles.empty())
    {
        return;
    }
    w.line("particles = [");
    for (const auto& layer : particles)
    {
        InlineTable table;
        table.add("kind", tomlQuote(enumToName<ParticleKind>(kParticleKindNames, layer.kind)));
        if (layer.color)
        {
            table.add("color", tomlQuote(layer.color->toString()));
        }
        if (layer.count)
        {
            table.add("count", std::to_string(*layer.count));
        }
        table.add("size", layer.size);
        table.add("speed", layer.speed);
        table.add("direction", layer.direction);
        if (layer.glow)
        {
            table.add("glow", *layer.glow ? "true" : "false");
        }
        w.line(std::format("    {},", table.take()));
    }
    w.line("]");
}

void writeWallpaper(Writer& w, const WallpaperConfig& wallpaper)
{
    w.header("wallpaper");
    w.string("mode", enumToName<WallpaperLayout>(kWallpaperLayoutNames, wallpaper.layout));
    if (wallpaper.background)
    {
        w.string("background", wallpaper.background->toHex());
    }
    if (wallpaper.layout == WallpaperLayout::Span)
    {
        w.string("source", wallpaper.spanSource);
    }
    const WallpaperPolicy& policy = wallpaper.policy;
    if (policy.pauseOnFullscreen || policy.pauseOnBattery || policy.pauseWhenCovered || policy.fpsLimit ||
        policy.mute)
    {
        w.header("wallpaper.policy");
        w.optional("pause_on_fullscreen", policy.pauseOnFullscreen);
        w.optional("pause_on_battery", policy.pauseOnBattery);
        w.optional("pause_when_covered", policy.pauseWhenCovered);
        w.optional("fps_limit", policy.fpsLimit);
        w.optional("mute", policy.mute);
    }
    if (wallpaper.layout == WallpaperLayout::Span)
    {
        return;
    }
    for (const auto& rule : wallpaper.monitors)
    {
        w.arrayHeader("wallpaper.monitor");
        w.string("match", rule.match.toString());
        w.string("kind", enumToName<WallpaperKind>(kWallpaperKindNames, rule.kind));
        if (rule.kind == WallpaperKind::Color)
        {
            if (rule.color)
            {
                w.string("color", rule.color->toHex());
            }
        }
        else
        {
            w.string("source", rule.source);
            w.string("fit", enumToName<WallpaperFit>(kWallpaperFitNames, rule.fit));
        }
        if (rule.kind == WallpaperKind::Video && !rule.fallbackImage.empty())
        {
            w.string("fallback_image", rule.fallbackImage);
        }
        if (rule.kind == WallpaperKind::Slideshow)
        {
            w.optional("interval", rule.intervalSeconds);
            w.optional("shuffle", rule.shuffle);
            continue;
        }
        writeEffects(w, rule.effects);
        writeParticles(w, rule.particles);
    }
}

void writeCursors(Writer& w, const CursorConfig& cursors)
{
    w.header("cursors");
    w.string("source", cursors.source);
    w.optional("size", cursors.size);
    if (!cursors.map.empty())
    {
        w.header("cursors.map");
        for (const auto& assignment : cursors.map)
        {
            w.string(riceName(assignment.role), assignment.file);
        }
    }
}

void writeSounds(Writer& w, const SoundsConfig& sounds)
{
    w.header("sounds");
    if (sounds.scheme)
    {
        w.string("scheme", enumToName<SoundScheme>(kSoundSchemeNames, *sounds.scheme));
    }
    if (!sounds.events.empty())
    {
        w.header("sounds.events");
        for (const auto& assignment : sounds.events)
        {
            w.string(riceName(assignment.event), assignment.file);
        }
    }
}

void writeIcons(Writer& w, const IconsConfig& icons)
{
    w.header("icons");
    for (const auto& [icon, file] : icons.desktop)
    {
        w.string(riceName(icon), file);
    }
    if (!icons.folders.empty())
    {
        w.header("icons.folders");
        for (const auto& [folder, file] : icons.folders)
        {
            w.string(riceName(folder), file);
        }
    }
    if (!icons.apps.empty())
    {
        w.header("icons.apps");
        for (const auto& [program, file] : icons.apps)
        {
            w.string(program, file); // a quoted key: it has a dot
        }
    }
    if (!icons.types.empty())
    {
        w.header("icons.types");
        for (const auto& [extension, file] : icons.types)
        {
            w.string(extension, file);
        }
    }
    if (!icons.machine.empty())
    {
        w.header("icons.machine");
        for (const auto& [icon, file] : icons.machine)
        {
            w.string(riceName(icon), file);
        }
    }
}

void writeLockScreen(Writer& w, const LockScreenConfig& lockscreen)
{
    w.header("lockscreen");
    if (!lockscreen.image.empty())
    {
        w.string("image", lockscreen.image);
    }
    if (lockscreen.spotlight)
    {
        w.boolean("spotlight", *lockscreen.spotlight);
    }
    if (lockscreen.tips)
    {
        w.boolean("tips", *lockscreen.tips);
    }
}

void writeWindows(Writer& w, const WindowsLookConfig& windows)
{
    w.header("windows");
    if (windows.corners)
    {
        w.string("corners", toString(*windows.corners));
    }
    if (windows.noBorder)
    {
        w.string("border", "none");
    }
    else if (windows.border)
    {
        w.string("border", windows.border->toString());
    }
    if (windows.caption)
    {
        w.string("caption", windows.caption->toString());
    }
    if (windows.title)
    {
        w.string("title", windows.title->toString());
    }
    if (windows.backdrop)
    {
        w.string("backdrop", toString(*windows.backdrop));
    }
    if (windows.dark)
    {
        w.boolean("dark", *windows.dark);
    }
    if (!windows.exclude.empty())
    {
        std::string list;
        for (const auto& name : windows.exclude)
        {
            list += (list.empty() ? "" : ", ") + tomlQuote(name);
        }
        w.key("exclude", "[" + list + "]");
    }
}

void writeTaskbar(Writer& w, const TaskbarConfig& taskbar)
{
    w.header("taskbar");
    if (taskbar.alignment)
    {
        w.string("alignment", enumToName<TaskbarAlignment>(kTaskbarAlignmentNames, *taskbar.alignment));
    }
    w.optional("autohide", taskbar.autohide);
    w.optional("task_view", taskbar.taskView);
    if (taskbar.search)
    {
        w.string("search", enumToName<TaskbarSearch>(kTaskbarSearchNames, *taskbar.search));
    }
}

void writeStart(Writer& w, const StartConfig& start)
{
    w.header("start");
    if (start.menu)
    {
        w.string("menu", enumToName<StartMenuKind>(kStartMenuNames, *start.menu));
    }
    if (!start.pinned.empty())
    {
        std::string list;
        for (const auto& id : start.pinned)
        {
            list += (list.empty() ? "" : ", ") + tomlQuote(id);
        }
        w.key("pinned", "[" + list + "]");
    }
    if (start.layout)
    {
        w.string("layout", enumToName<StartLayout>(kStartLayoutNames, *start.layout));
    }
    w.optional("recommendations", start.recommendations);
}

void writePlugins(Writer& w, const Rice& rice)
{
    if (rice.pluginRequirements.empty() && rice.pluginSettings.empty())
    {
        return;
    }
    w.header("plugins");
    if (!rice.pluginRequirements.empty())
    {
        std::string list;
        for (const auto& requirement : rice.pluginRequirements)
        {
            list += list.empty() ? "" : ", ";
            const std::string version = requirement.version.toString();
            list += tomlQuote(version.empty() || version == "*" ? requirement.id
                                                                : requirement.id + "@" + version);
        }
        w.key("requires", "[" + list + "]");
    }
    for (const auto& settings : rice.pluginSettings)
    {
        w.header("plugins." + tomlKey(settings.id));
        if (const auto* members = settings.config.table())
        {
            for (const auto& member : *members)
            {
                if (!member.value.isNull())
                {
                    w.key(member.key, tomlValue(member.value));
                }
            }
        }
    }
}

std::string barItems(const std::vector<BarItem>& items)
{
    std::string list;
    for (const auto& item : items)
    {
        list += list.empty() ? "" : ", ";
        const auto* config = item.config.table();
        if (config == nullptr)
        {
            list += tomlQuote(item.id);
            continue;
        }
        list += "{ id = " + tomlQuote(item.id);
        for (const auto& member : *config)
        {
            if (!member.value.isNull())
            {
                list += ", " + tomlKey(member.key) + " = " + tomlValue(member.value);
            }
        }
        list += " }";
    }
    return "[" + list + "]";
}

void writeBar(Writer& w, const BarConfig& bar)
{
    w.arrayHeader("bar");
    w.string("monitor", bar.monitor.toString());
    if (!bar.skin.empty())
    {
        w.string("skin", bar.skin);
    }
    if (bar.style)
    {
        w.string("style", enumToName<BarStyle>(kBarStyleNames, *bar.style));
    }
    // What the bar itself sets: the keys it was read with; a new bar without a skin sets them all.
    const bool all = bar.keys.empty() && bar.skin.empty() && !bar.style;
    const auto has = [&](std::string_view key) { return all || std::ranges::contains(bar.keys, key); };
    if (has("position"))
    {
        w.string("position", enumToName<BarPosition>(kBarPositionNames, bar.position));
    }
    if (has("mode"))
    {
        w.string("mode", enumToName<BarMode>(kBarModeNames, bar.mode));
    }
    if (has("thickness"))
    {
        w.integer("thickness", bar.thickness);
    }
    if (has("length"))
    {
        if (bar.length.fit)
        {
            w.string("length", "auto");
        }
        else if (bar.length.percent)
        {
            w.string("length", std::format("{}%", bar.length.value));
        }
        else
        {
            w.integer("length", static_cast<std::int64_t>(std::llround(bar.length.value)));
        }
    }
    if (has("margin"))
    {
        w.key("margin",
              std::format("[{}, {}, {}, {}]", bar.margin[0], bar.margin[1], bar.margin[2], bar.margin[3]));
    }
    if (has("radius"))
    {
        w.integer("radius", bar.radius);
    }
    if (has("background"))
    {
        std::string background =
            "{ kind = " +
            tomlQuote(enumToName<BarBackground::Kind>(kBarBackgroundNames, bar.background.kind));
        if (bar.background.tint)
        {
            background += ", tint = " + tomlQuote(bar.background.tint->toString());
        }
        if (bar.background.opacity)
        {
            background += ", opacity = " + number(*bar.background.opacity);
        }
        w.key("background", background + " }");
    }
    if (has("foreground") && bar.foreground)
    {
        w.string("foreground", bar.foreground->toString());
    }
    if (has("autohide"))
    {
        w.boolean("autohide", bar.autohide);
    }
    if (has("hide_native_taskbar"))
    {
        w.boolean("hide_native_taskbar", bar.hideNativeTaskbar);
    }
    if (has("left"))
    {
        w.key("left", barItems(bar.left));
    }
    if (has("center"))
    {
        w.key("center", barItems(bar.center));
    }
    if (has("right"))
    {
        w.key("right", barItems(bar.right));
    }
}

} // namespace

std::string tomlQuote(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20 || byte == 0x7F)
            {
                out += std::format("\\u{:04X}", static_cast<unsigned>(byte));
            }
            else
            {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string tomlKey(std::string_view key)
{
    return isBareKey(key) ? std::string(key) : tomlQuote(key);
}

std::string tomlValue(const Value& value)
{
    struct Visitor
    {
        std::string operator()(std::monostate) const { return "{}"; }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(std::int64_t i) const { return std::to_string(i); }
        std::string operator()(double d) const { return number(d); }
        std::string operator()(const std::string& s) const { return tomlQuote(s); }
        std::string operator()(const Value::Array& array) const
        {
            std::string out = "[";
            for (const auto& item : array)
            {
                if (item.isNull())
                {
                    continue;
                }
                out += out.size() > 1 ? ", " : "";
                out += tomlValue(item);
            }
            return out + "]";
        }
        std::string operator()(const Value::Table& table) const
        {
            std::string out = "{";
            bool first = true;
            for (const auto& member : table)
            {
                if (member.value.isNull())
                {
                    continue;
                }
                out += first ? " " : ", ";
                first = false;
                out += tomlKey(member.key) + " = " + tomlValue(member.value);
            }
            return out + (first ? "}" : " }");
        }
    };
    return std::visit(Visitor{}, value.data);
}

std::string writeRice(const Rice& rice)
{
    Writer w;
    w.integer("schema", rice.schema);
    writeMeta(w, rice.meta);
    if (!rice.palette.empty())
    {
        w.header("palette");
        for (const auto& entry : rice.palette)
        {
            w.string(entry.name, entry.color.toHex());
        }
    }
    if (rice.appearance)
    {
        writeAppearance(w, *rice.appearance);
    }
    if (rice.wallpaper)
    {
        writeWallpaper(w, *rice.wallpaper);
    }
    if (rice.cursors)
    {
        writeCursors(w, *rice.cursors);
    }
    if (rice.sounds)
    {
        writeSounds(w, *rice.sounds);
    }
    if (rice.icons)
    {
        writeIcons(w, *rice.icons);
    }
    if (rice.lockscreen)
    {
        writeLockScreen(w, *rice.lockscreen);
    }
    if (rice.windows)
    {
        writeWindows(w, *rice.windows);
    }
    if (rice.taskbar)
    {
        writeTaskbar(w, *rice.taskbar);
    }
    if (rice.start)
    {
        writeStart(w, *rice.start);
    }
    writePlugins(w, rice);
    for (const auto& bar : rice.bars)
    {
        writeBar(w, bar);
    }
    return w.take();
}

} // namespace rmw::domain
