#pragma once

#include "rmw/ports/Icons.hpp"
#include "rmw/ports/LockScreen.hpp"
#include "rmw/ports/Registry.hpp"
#include "rmw/ports/Shell.hpp"

#include <algorithm>
#include <map>
#include <set>
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

    // Keys are implicit here: an "empty" key is one no value lives in or under.
    Result<void> removeKeyIfEmpty(std::string_view key) override
    {
        const std::string prefix = std::string(key) + "\\";
        const bool used = std::ranges::any_of(values, [&](const auto& entry) {
            return entry.first.first == key || entry.first.first.starts_with(prefix);
        });
        if (!used)
        {
            removedKeys.emplace_back(key);
        }
        return {};
    }
    std::vector<std::string> removedKeys;

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
    bool isWindowsFile(std::string_view file) override
    {
        return file.starts_with(R"(C:\Windows\)") || file.starts_with("%SystemRoot%");
    }
    bool fileExists(std::string_view file) override { return !gone.contains(std::string(file)); }
    Result<bool> taskbarAutoHide() override { return autohide; }
    Result<void> setTaskbarAutoHide(bool enabled) override
    {
        autohide = enabled;
        return {};
    }

    std::vector<std::string> broadcasts;
    std::set<std::string, std::less<>> gone; // files deleted since they were captured
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
        m_registry.values[{R"(Software\Microsoft\Windows\CurrentVersion\Themes)", "CurrentTheme"}] =
            ports::RegistryValue::fromString(R"(C:\Users\me\AppData\Local\Microsoft\Windows\Themes\RicingManager.theme)");
        return {};
    }

    // The theme file's look comes with it (set by the test in `looks`).
    Result<void> applyThemeFile(const std::string& file) override
    {
        files.push_back(file);
        const auto found = looks.find(file);
        if (found == looks.end())
        {
            return rmw::fail(ErrorCode::NotFound, "no such theme (fake)");
        }
        constexpr std::string_view personalize =
            R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
        m_registry.setDword(personalize, "AppsUseLightTheme", found->second.appsLight ? 1 : 0);
        m_registry.setDword(personalize, "SystemUsesLightTheme", found->second.systemLight ? 1 : 0);
        m_registry.setDword(R"(Software\Microsoft\Windows\DWM)", "ColorizationColor", found->second.colorizationArgb);
        m_registry.values[{R"(Software\Microsoft\Windows\CurrentVersion\Themes)", "CurrentTheme"}] =
            ports::RegistryValue::fromString(file);
        return {};
    }

    Result<void> nameCurrentTheme(const std::string& file) override
    {
        named.push_back(file);
        m_registry.values[{R"(Software\Microsoft\Windows\CurrentVersion\Themes)", "CurrentTheme"}] =
            ports::RegistryValue::fromString(file);
        return {};
    }

    std::vector<std::string> files;
    std::vector<std::string> named;
    std::map<std::string, ports::ThemeLook> looks;

    std::vector<ports::ThemeLook> applied;
    bool fail = false;

private:
    FakeRegistry& m_registry;
};

// The user's folders and shortcuts, in memory (M14).
class FakeLockScreen final : public ports::ILockScreen
{
public:
    std::string source;          // the file Windows names ("" none)
    std::string shown = "saved"; // what currentImage() gives when the source is not there
    std::set<std::string, std::less<>> gone;
    int sets = 0;
    bool broken = false;         // the API fails

    Result<std::string> currentImage() override
    {
        if (broken)
        {
            return fail(ErrorCode::System, "no lock screen");
        }
        return !source.empty() && exists(source) ? source : shown;
    }
    Result<std::string> imageSource() override { return source; }
    Result<void> setImage(const std::string& file) override
    {
        source = file;
        ++sets;
        return {};
    }
    bool exists(const std::string& file) override { return !gone.contains(file); }
};

class FakeIcons final : public ports::IIcons
{
public:
    std::map<std::string, std::string, std::less<>> folders; // folder -> icon (absent: none)
    std::vector<ports::Shortcut> links;
    int refreshes = 0;
    int shortcutWrites = 0;

    Result<std::optional<std::string>> folderIcon(std::string_view folder) override
    {
        const auto found = folders.find(folder);
        return found == folders.end() ? std::optional<std::string>() : std::optional(found->second);
    }
    Result<void> setFolderIcon(std::string_view folder, const std::optional<std::string>& icon) override
    {
        if (icon)
        {
            folders[std::string(folder)] = *icon;
        }
        else
        {
            folders.erase(std::string(folder));
        }
        return {};
    }
    Result<std::vector<ports::Shortcut>> shortcuts() override { return links; }
    Result<void> setShortcutIcon(const std::string& path, const std::string& icon) override
    {
        for (auto& link : links)
        {
            if (link.path == path)
            {
                link.icon = icon;
                ++shortcutWrites;
                return {};
            }
        }
        return fail(ErrorCode::NotFound, path);
    }
    Result<std::string> progIdOf(std::string_view extension) override
    {
        const auto found = progIds.find(std::string(extension));
        return found == progIds.end() ? std::string() : found->second;
    }
    bool exists(std::string_view location) override { return !gone.contains(std::string(location)); }
    void refresh() override { ++refreshes; }

    std::map<std::string, std::string> progIds; // extension -> ProgID

    std::set<std::string, std::less<>> gone; // icon locations whose file is not there
};
} // namespace rmw::test
