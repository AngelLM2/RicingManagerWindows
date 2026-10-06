#include "rmw/modules/Cursors.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/CursorFiles.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/modules/RegistryFields.hpp"

#include <array>
#include <format>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr std::string_view kCursors = R"(Control Panel\Cursors)";
constexpr std::array<std::string_view, 1> kDependsOn = {"appearance"}; // a .theme can carry cursors
constexpr DwordField kSchemeSource = {"schemeSource", kCursors, "Scheme Source"};
constexpr DwordField kSize = {"size", kCursors, "CursorBaseSize"};
constexpr std::uint32_t kDefaultSize = 32;

Json stringField(const std::optional<ports::RegistryValue>& value)
{
    if (!value || (value->type != ports::RegistryValue::Type::String &&
                   value->type != ports::RegistryValue::Type::ExpandString))
    {
        return nullptr;
    }
    return Json{{"type", value->type == ports::RegistryValue::Type::String ? "string" : "expand"},
                {"data", value->text}};
}

Result<void> writeStringField(ports::IRegistry& registry, ports::IShell& shell, std::string_view name,
                              const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(kCursors, name);
    }
    const auto type = value.find("type");
    const auto data = value.find("data");
    if (!value.is_object() || type == value.end() || data == value.end() || !type->is_string() ||
        !data->is_string())
    {
        return fail(ErrorCode::InvalidArgument, std::format("cursor field '{}' is malformed", name));
    }
    // The file the user had while it is there, else its copy in the store.
    const std::string copy = json::stringOr(value, "copy", "");
    const std::string& text =
        copy.empty() || shell.fileExists(data->get_ref<const std::string&>()) ? data->get_ref<const std::string&>() : copy;
    return registry.write(kCursors, name,
                          *type == "string" ? ports::RegistryValue::fromString(text)
                                            : ports::RegistryValue::fromExpandString(text));
}

std::string_view extensionOf(std::string_view path) noexcept
{
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
}

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(text[i]) != lower(prefix[i]))
        {
            return false;
        }
    }
    return true;
}

// install.inf comes as ANSI, UTF-8 or UTF-16 LE with a BOM.
Result<std::string> decodeInf(const std::vector<std::byte>& bytes)
{
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    if (bytes.size() >= 2 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xFE})
    {
        const std::wstring_view wide(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                                     (bytes.size() - 2) / 2);
        return utf16To8(wide);
    }
    std::string_view text(data, bytes.size());
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3);
    }
    return std::string(text);
}

} // namespace

std::span<const std::string_view> CursorsLogic::dependsOn() const noexcept
{
    return kDependsOn;
}

Result<Json> CursorsLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const
{
    Json desired = Json::object();
    if (!rice.cursors)
    {
        return desired;
    }
    const domain::CursorConfig& config = *rice.cursors;

    std::vector<domain::CursorAssignment> assignments = config.map;
    if (assignments.empty())
    {
        auto inf = assets.readPackageFile(config.source + "/install.inf", limits::kInfMaxBytes);
        if (!inf)
        {
            return std::unexpected(
                std::move(inf.error()).withContext("cursors: no [cursors.map] and no install.inf"));
        }
        auto text = decodeInf(*inf);
        if (!text)
        {
            return std::unexpected(std::move(text.error()).withContext("install.inf"));
        }
        auto parsed = domain::parseCursorInf(*text);
        if (!parsed)
        {
            return std::unexpected(std::move(parsed.error()));
        }
        assignments = std::move(*parsed);
    }

    for (const auto& assignment : assignments)
    {
        auto path = domain::validatePackagePath(config.source + "/" + assignment.file);
        if (!path)
        {
            return std::unexpected(std::move(path.error()));
        }
        const std::string context = std::format("cursor '{}'", *path);
        auto bytes = assets.readPackageFile(*path, limits::kCursorMaxBytes);
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()).withContext(context));
        }
        if (auto valid = domain::validateCursorFile(*bytes, extensionOf(*path)); !valid)
        {
            return std::unexpected(std::move(valid.error()).withContext(context));
        }
        auto stored = assets.fromPackage(*path);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        const std::u8string storedPath = stored->u8string();
        desired[std::string(domain::registryValueName(assignment.role))] =
            Json{{"type", "expand"}, {"data", std::string(storedPath.begin(), storedPath.end())}};
    }
    if (!assignments.empty())
    {
        // A custom set, not a named Windows scheme: Settings shows "(None)".
        desired["scheme"] = Json{{"type", "string"}, {"data", ""}};
        desired["schemeSource"] = 0;
    }
    if (config.size)
    {
        desired["size"] = *config.size;
    }
    return desired;
}

Result<void> CursorsLogic::internalize(Json& state, engine::IAssetSink& assets) const
{
    for (std::size_t i = 0; i < domain::kCursorRoleCount; ++i)
    {
        const std::string name(domain::registryValueName(static_cast<domain::CursorRole>(i)));
        const auto field = state.find(name);
        if (field == state.end() || !field->is_object() || !field->contains("data") ||
            !(*field)["data"].is_string())
        {
            continue;
        }
        const std::string path = (*field)["data"].get<std::string>();
        const bool keep = json::boolOr(*field, "keep", false);
        field->erase("keep");
        // Windows' own cursors never go away; anything with %VARS% we cannot resolve here.
        if (keep || path.empty() || startsWithIgnoreCase(path, "%SystemRoot%") ||
            startsWithIgnoreCase(path, "%windir%") || path.find('%') != std::string::npos)
        {
            continue;
        }
        auto wide = utf8To16(path);
        if (!wide)
        {
            continue;
        }
        auto stored = assets.fromSystem(std::filesystem::path(*wide), extensionOf(path));
        if (!stored)
        {
            // Missing or unreadable: keep the original path; a restore puts back what was there.
            log::warn("cursors: cannot keep a copy of {}: {}", path, stored.error().message());
            continue;
        }
        const std::u8string storedPath = stored->u8string();
        (*field)["copy"] = std::string(storedPath.begin(), storedPath.end());
    }
    return {};
}

bool CursorsLogic::fieldEquivalent(std::string_view /*field*/, const Json& a, const Json& b) const
{
    const auto plain = [](Json value) {
        if (value.is_object())
        {
            value.erase("copy");
        }
        return value;
    };
    return plain(a) == plain(b);
}

Result<engine::CaptureResult> CursorsSystem::capture()
{
    engine::CaptureResult result;
    for (std::size_t i = 0; i < domain::kCursorRoleCount; ++i)
    {
        const std::string_view name = domain::registryValueName(static_cast<domain::CursorRole>(i));
        auto value = m_registry.read(kCursors, name);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        Json field = stringField(*value);
        if (field.is_object() && m_shell.isWindowsFile(field["data"].get<std::string>()))
        {
            field["keep"] = true; // Windows' own cursor: never copied
        }
        result.state[std::string(name)] = std::move(field);
    }
    auto scheme = m_registry.read(kCursors, "");
    if (!scheme)
    {
        return std::unexpected(std::move(scheme.error()));
    }
    result.state["scheme"] = stringField(*scheme);
    for (const auto& field : {kSchemeSource, kSize})
    {
        auto value = readDwordField(m_registry, field);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[std::string(field.field)] = std::move(*value);
    }
    return result;
}

Result<void> CursorsSystem::apply(const Json& desired)
{
    bool schemeChanged = false;
    for (std::size_t i = 0; i < domain::kCursorRoleCount; ++i)
    {
        const std::string_view name = domain::registryValueName(static_cast<domain::CursorRole>(i));
        if (const auto value = desired.find(name); value != desired.end())
        {
            if (auto written = writeStringField(m_registry, m_shell, name, *value); !written)
            {
                return written;
            }
            schemeChanged = true;
        }
    }
    if (const auto scheme = desired.find("scheme"); scheme != desired.end())
    {
        if (auto written = writeStringField(m_registry, m_shell, "", *scheme); !written)
        {
            return written;
        }
        schemeChanged = true;
    }
    if (const auto source = desired.find(kSchemeSource.field); source != desired.end())
    {
        if (auto written = writeDwordField(m_registry, kSchemeSource, *source); !written)
        {
            return written;
        }
    }
    if (schemeChanged)
    {
        if (auto reloaded = m_shell.reloadCursors(); !reloaded)
        {
            return reloaded;
        }
    }
    if (const auto size = desired.find(kSize.field); size != desired.end())
    {
        if (!size->is_null() && !size->is_number_unsigned() && !size->is_number_integer())
        {
            return fail(ErrorCode::InvalidArgument, "field 'size' needs a number or null");
        }
        const auto pixels = size->is_null() ? kDefaultSize : size->get<std::uint32_t>();
        if (auto set = m_shell.setCursorSize(pixels); !set)
        {
            return set;
        }
        // Absent before: remove it again after Windows wrote the default.
        if (auto written = writeDwordField(m_registry, kSize, *size); !written)
        {
            return written;
        }
    }
    return {};
}

} // namespace rmw::modules
