#pragma once

#include "rmw/base/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rmw::ports
{

struct RegistryValue
{
    enum class Type
    {
        Dword,
        String,
        ExpandString, // REG_EXPAND_SZ: Windows expands %VARS% when reading (cursor paths)
        Binary, // REG_BINARY, or the bytes of any other type (see registryType)
    };

    Type type = Type::Dword;
    std::uint32_t dword = 0;
    std::string text; // UTF-8
    std::vector<std::byte> bytes;
    // Binary only: the value's own registry type when it is not REG_BINARY (REG_QWORD, REG_MULTI_SZ,
    // REG_NONE...), so it is written back as it was read.
    std::optional<std::uint32_t> registryType;

    static RegistryValue fromDword(std::uint32_t value) { return {Type::Dword, value, {}, {}, {}}; }
    static RegistryValue fromString(std::string value) { return {Type::String, 0, std::move(value), {}, {}}; }
    static RegistryValue fromExpandString(std::string value)
    {
        return {Type::ExpandString, 0, std::move(value), {}, {}};
    }

    friend bool operator==(const RegistryValue&, const RegistryValue&) = default;
};

// HKEY_CURRENT_USER only: the app never writes machine-wide settings. `key` is a subkey path
// such as R"(Control Panel\Cursors)"; `name` empty means the key's default value.
class IRegistry
{
public:
    virtual ~IRegistry() = default;

    // nullopt when the key or the value does not exist.
    [[nodiscard]] virtual Result<std::optional<RegistryValue>> read(std::string_view key,
                                                                    std::string_view name) = 0;
    // Creates the key when needed.
    [[nodiscard]] virtual Result<void> write(std::string_view key, std::string_view name,
                                             const RegistryValue& value) = 0;
    // Succeeds when the value is already absent.
    [[nodiscard]] virtual Result<void> remove(std::string_view key, std::string_view name) = 0;
    // Deletes the key when it holds no value and no subkey (what a restore leaves of a key that was
    // not there before); succeeds when it is not empty or not there.
    [[nodiscard]] virtual Result<void> removeKeyIfEmpty(std::string_view key) = 0;
};

} // namespace rmw::ports
