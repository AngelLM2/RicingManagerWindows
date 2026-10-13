#include "rmw/base/Utf.hpp"
#include "rmw/platform/Win32Ports.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <array>
#include <cstring>
#include <format>
#include <vector>

namespace rmw::platform
{
namespace
{

constexpr DWORD kMaxValueBytes = 1u << 20;

Result<std::wstring> wide(std::string_view text)
{
    return utf8To16(text);
}

// REG_SZ data without its terminating NULs.
std::wstring_view trimNuls(const wchar_t* data, std::size_t chars)
{
    std::wstring_view text(data, chars);
    while (!text.empty() && text.back() == L'\0')
    {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

Win32Registry::Win32Registry(std::wstring root)
    : m_root(std::move(root))
{
}

Result<std::wstring> Win32Registry::fullKey(std::string_view key) const
{
    auto subkey = wide(key);
    if (!subkey)
    {
        return subkey;
    }
    if (m_root.empty())
    {
        return subkey;
    }
    return subkey->empty() ? m_root : m_root + L"\\" + *subkey;
}

Result<std::optional<ports::RegistryValue>> Win32Registry::read(std::string_view key, std::string_view name)
{
    auto path = fullKey(key);
    auto valueName = wide(name);
    if (!path || !valueName)
    {
        return fail(ErrorCode::InvalidArgument, "registry path is not valid UTF-8");
    }

    wil::unique_hkey hkey;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path->c_str(), 0, KEY_QUERY_VALUE, hkey.put());
    if (status == ERROR_FILE_NOT_FOUND)
    {
        return std::optional<ports::RegistryValue>{};
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "open registry key"));
    }

    const wchar_t* namePtr = valueName->empty() ? nullptr : valueName->c_str();
    DWORD type = 0;
    DWORD size = 0;
    status = RegQueryValueExW(hkey.get(), namePtr, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND)
    {
        return std::optional<ports::RegistryValue>{};
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "query registry value"));
    }
    if (size > kMaxValueBytes)
    {
        return fail(ErrorCode::LimitExceeded, std::format("registry value '{}' is too large", name));
    }

    // +2 so a string that is not NUL-terminated still reads safely.
    std::vector<std::byte> data(static_cast<std::size_t>(size) + sizeof(wchar_t));
    status =
        RegQueryValueExW(hkey.get(), namePtr, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size);
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "read registry value"));
    }

    ports::RegistryValue value;
    switch (type)
    {
    case REG_DWORD:
        if (size != sizeof(DWORD))
        {
            return fail(ErrorCode::InvalidData,
                        std::format("registry value '{}' is a malformed DWORD", name));
        }
        value.type = ports::RegistryValue::Type::Dword;
        std::memcpy(&value.dword, data.data(), sizeof(DWORD));
        break;
    case REG_SZ:
    case REG_EXPAND_SZ: {
        value.type =
            type == REG_SZ ? ports::RegistryValue::Type::String : ports::RegistryValue::Type::ExpandString;
        const auto text = trimNuls(reinterpret_cast<const wchar_t*>(data.data()), size / sizeof(wchar_t));
        auto utf8 = utf16To8(text);
        if (!utf8)
        {
            return std::unexpected(
                std::move(utf8.error()).withContext(std::format("registry value '{}'", name)));
        }
        value.text = std::move(*utf8);
        break;
    }
    default:
        // REG_QWORD, REG_MULTI_SZ... as their bytes, with their type: written back as REG_BINARY they
        // were another value (Explorer reads some only with their own type).
        value.type = ports::RegistryValue::Type::Binary;
        if (type != REG_BINARY)
        {
            value.registryType = type;
        }
        data.resize(size);
        value.bytes = std::move(data);
        break;
    }
    return std::optional<ports::RegistryValue>(std::move(value));
}

Result<void> Win32Registry::write(std::string_view key, std::string_view name,
                                  const ports::RegistryValue& value)
{
    auto path = fullKey(key);
    auto valueName = wide(name);
    if (!path || !valueName)
    {
        return fail(ErrorCode::InvalidArgument, "registry path is not valid UTF-8");
    }
    wil::unique_hkey hkey;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path->c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                     KEY_SET_VALUE, nullptr, hkey.put(), nullptr);
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "create registry key"));
    }
    const wchar_t* namePtr = valueName->empty() ? nullptr : valueName->c_str();

    switch (value.type)
    {
    case ports::RegistryValue::Type::Dword: {
        const DWORD data = value.dword;
        status = RegSetValueExW(hkey.get(), namePtr, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data),
                                sizeof(data));
        break;
    }
    case ports::RegistryValue::Type::String:
    case ports::RegistryValue::Type::ExpandString: {
        auto text = utf8To16(value.text);
        if (!text)
        {
            return std::unexpected(std::move(text.error()));
        }
        const auto bytes = static_cast<DWORD>((text->size() + 1) * sizeof(wchar_t));
        status = RegSetValueExW(hkey.get(), namePtr, 0,
                                value.type == ports::RegistryValue::Type::String ? REG_SZ : REG_EXPAND_SZ,
                                reinterpret_cast<const BYTE*>(text->c_str()), bytes);
        break;
    }
    case ports::RegistryValue::Type::Binary:
        if (value.bytes.size() > kMaxValueBytes)
        {
            return fail(ErrorCode::LimitExceeded, "registry value too large");
        }
        status = RegSetValueExW(hkey.get(), namePtr, 0, value.registryType.value_or(REG_BINARY),
                                reinterpret_cast<const BYTE*>(value.bytes.data()),
                                static_cast<DWORD>(value.bytes.size()));
        break;
    }
    if (status != ERROR_SUCCESS)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "write registry value"));
    }
    return {};
}

Result<void> Win32Registry::remove(std::string_view key, std::string_view name)
{
    auto path = fullKey(key);
    auto valueName = wide(name);
    if (!path || !valueName)
    {
        return fail(ErrorCode::InvalidArgument, "registry path is not valid UTF-8");
    }
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, path->c_str(),
                                              valueName->empty() ? nullptr : valueName->c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "delete registry value"));
    }
    return {};
}

Result<void> Win32Registry::removeKeyIfEmpty(std::string_view key)
{
    auto path = fullKey(key);
    if (!path)
    {
        return fail(ErrorCode::InvalidArgument, "registry path is not valid UTF-8");
    }
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path->c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
    {
        return {}; // not there
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    const LSTATUS info = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                          nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(handle);
    if (info != ERROR_SUCCESS || subkeys != 0 || values != 0)
    {
        return {};
    }
    const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, path->c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "delete an empty registry key"));
    }
    return {};
}

Result<std::vector<std::string>> Win32Registry::subkeys(std::string_view key, std::size_t limit)
{
    auto path = fullKey(key);
    if (!path)
    {
        return fail(ErrorCode::InvalidArgument, "registry path is not valid UTF-8");
    }
    std::vector<std::string> names;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path->c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &handle) != ERROR_SUCCESS)
    {
        return names; // not there
    }
    for (DWORD i = 0; names.size() < limit; ++i)
    {
        std::array<wchar_t, 256> name{}; // a key name is at most 255 characters
        auto size = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExW(handle, i, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
        {
            break;
        }
        if (auto utf8 = utf16To8(std::wstring_view(name.data(), size)))
        {
            names.push_back(std::move(*utf8));
        }
    }
    RegCloseKey(handle);
    return names;
}

Result<void> Win32Registry::deleteRoot()
{
    if (m_root.empty())
    {
        return {};
    }
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, m_root.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(status), "delete registry root"));
    }
    // RegDeleteTree empties the key; remove the key itself too.
    const LSTATUS keyStatus = RegDeleteKeyW(HKEY_CURRENT_USER, m_root.c_str());
    if (keyStatus != ERROR_SUCCESS && keyStatus != ERROR_FILE_NOT_FOUND)
    {
        return std::unexpected(win32Error(static_cast<std::uint32_t>(keyStatus), "delete registry root"));
    }
    return {};
}

} // namespace rmw::platform
