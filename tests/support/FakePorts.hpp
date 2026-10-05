#pragma once

#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rmw::test
{

// HKCU in memory. Keys compare case-sensitively here; the modules always use one spelling.
class FakeRegistry final : public ports::IRegistry
{
public:
    using Key = std::pair<std::string, std::string>; // subkey, value name

    Result<std::optional<ports::RegistryValue>> read(std::string_view key, std::string_view name) override
    {
        if (failReads)
        {
            return fail(ErrorCode::System, "registry read failed (injected)");
        }
        const auto it = values.find({std::string(key), std::string(name)});
        if (it == values.end())
        {
            return std::optional<ports::RegistryValue>{};
        }
        return std::optional<ports::RegistryValue>(it->second);
    }

    Result<void> write(std::string_view key, std::string_view name,
                       const ports::RegistryValue& value) override
    {
        if (failWrites)
        {
            return fail(ErrorCode::AccessDenied, "registry write failed (injected)");
        }
        values[{std::string(key), std::string(name)}] = value;
        return {};
    }

    Result<void> remove(std::string_view key, std::string_view name) override
    {
        values.erase({std::string(key), std::string(name)});
        return {};
    }

    void setDword(std::string_view key, std::string_view name, std::uint32_t value)
    {
        values[{std::string(key), std::string(name)}] = ports::RegistryValue::fromDword(value);
    }
    std::optional<std::uint32_t> dword(std::string_view key, std::string_view name) const
    {
        const auto it = values.find({std::string(key), std::string(name)});
        if (it == values.end())
        {
            return std::nullopt;
        }
        return it->second.dword;
    }

    std::map<Key, ports::RegistryValue> values;
    bool failReads = false;
    bool failWrites = false;
};

class FakeShell final : public ports::IShell
{
public:
    Result<void> broadcastSettingChange(std::string_view area) override
    {
        broadcasts.emplace_back(area);
        return {};
    }
    Result<void> reloadCursors() override
    {
        ++cursorReloads;
        return {};
    }
    Result<void> setCursorSize(std::uint32_t pixels) override
    {
        cursorSize = pixels;
        return {};
    }
    Result<bool> taskbarAutoHide() override { return autohide; }
    Result<void> setTaskbarAutoHide(bool enabled) override
    {
        autohide = enabled;
        return {};
    }

    std::vector<std::string> broadcasts;
    int cursorReloads = 0;
    std::uint32_t cursorSize = 32;
    bool autohide = false;
};

// Behaves like IThemeManager2 does on success: Windows rewrites these registry values.
class FakeThemeApi final : public ports::IThemeApi
{
public:
    explicit FakeThemeApi(FakeRegistry& registry)
        : m_registry(registry)
    {
    }

    Result<void> applyLook(const ports::ThemeLook& look) override
    {
        applied.push_back(look);
        if (fail)
        {
            return rmw::fail(ErrorCode::Timeout, "theme did not apply (injected)");
        }
        constexpr std::string_view personalize =
            R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
        m_registry.setDword(personalize, "AppsUseLightTheme", look.appsLight ? 1 : 0);
        m_registry.setDword(personalize, "SystemUsesLightTheme", look.systemLight ? 1 : 0);
        m_registry.setDword(R"(Software\Microsoft\Windows\DWM)", "ColorizationColor", look.colorizationArgb);
        return {};
    }

    std::vector<ports::ThemeLook> applied;
    bool fail = false;

private:
    FakeRegistry& m_registry;
};

} // namespace rmw::test
