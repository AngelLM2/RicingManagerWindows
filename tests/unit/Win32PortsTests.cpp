#include "rmw/domain/MonitorSelector.hpp"
#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace
{

using rmw::platform::buildManagedTheme;
using rmw::platform::Win32Registry;
using rmw::ports::RegistryValue;
using rmw::ports::ThemeLook;

// Every test gets its own key under the app's own HKCU area, deleted afterwards.
class Win32RegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto suffix = rmw::platform::randomHex(8);
        ASSERT_TRUE(suffix.has_value());
        m_registry.emplace(LR"(Software\RicingManagerWindows\Tests\)" +
                           std::wstring(suffix->begin(), suffix->end()));
    }
    void TearDown() override
    {
        EXPECT_TRUE(m_registry->deleteRoot().has_value());
        // Parents too, but only while empty (RegDeleteKey refuses keys with subkeys).
        RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\RicingManagerWindows\Tests)");
        RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\RicingManagerWindows)");
    }

    std::optional<Win32Registry> m_registry;
};

TEST_F(Win32RegistryTest, RoundTripsEveryValueType)
{
    constexpr std::string_view key = R"(Control Panel\Cursors)";
    ASSERT_TRUE(m_registry->write(key, "Size", RegistryValue::fromDword(48)));
    ASSERT_TRUE(
        m_registry->write(key, "Arrow", RegistryValue::fromExpandString(R"(%LOCALAPPDATA%\夜景\a.cur)")));
    ASSERT_TRUE(m_registry->write(key, "", RegistryValue::fromString("Ricing Manager")));

    EXPECT_EQ(m_registry->read(key, "Size").value(), RegistryValue::fromDword(48));
    EXPECT_EQ(m_registry->read(key, "Arrow").value(),
              RegistryValue::fromExpandString(R"(%LOCALAPPDATA%\夜景\a.cur)"));
    EXPECT_EQ(m_registry->read(key, "").value(), RegistryValue::fromString("Ricing Manager"));
}

// The review of v1.7: a REG_QWORD (or REG_MULTI_SZ) was read as bytes and written back as REG_BINARY.
TEST(Win32Registry, OtherTypesComeBackWithTheirType)
{
    const auto suffix = rmw::platform::randomHex(8);
    ASSERT_TRUE(suffix.has_value());
    const std::wstring root = LR"(Software\RicingManagerWindows\Tests\)" + std::wstring(suffix->begin(), suffix->end());
    Win32Registry registry(root);
    const std::wstring key = root + L"\\Q";
    const std::uint64_t big = 0x0123'4567'89AB'CDEFULL;
    ASSERT_EQ(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"Big", REG_QWORD, &big, sizeof(big)), ERROR_SUCCESS);

    const auto read = registry.read("Q", "Big").value();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->type, RegistryValue::Type::Binary);
    EXPECT_EQ(read->registryType, std::optional<std::uint32_t>(REG_QWORD));
    ASSERT_TRUE(registry.write("Q", "Copy", *read));
    DWORD type = 0;
    std::uint64_t copy = 0;
    DWORD size = sizeof(copy);
    EXPECT_EQ(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Copy", RRF_RT_ANY, &type, &copy, &size), ERROR_SUCCESS);
    EXPECT_EQ(type, static_cast<DWORD>(REG_QWORD));
    EXPECT_EQ(copy, big);

    EXPECT_TRUE(registry.deleteRoot().has_value());
    RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\RicingManagerWindows\Tests)");
    RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\RicingManagerWindows)");
}

TEST_F(Win32RegistryTest, MissingKeysAndValuesReadAsNothing)
{
    EXPECT_FALSE(m_registry->read("Nope", "Nothing").value().has_value());
    ASSERT_TRUE(m_registry->write("K", "A", RegistryValue::fromDword(1)));
    EXPECT_FALSE(m_registry->read("K", "B").value().has_value());

    ASSERT_TRUE(m_registry->remove("K", "A"));
    EXPECT_FALSE(m_registry->read("K", "A").value().has_value());
    EXPECT_TRUE(m_registry->remove("K", "A").has_value()); // already gone: fine
    EXPECT_TRUE(m_registry->remove("Nope", "A").has_value());
}

std::vector<std::byte> edidSample(std::uint32_t serial, std::string_view serialText)
{
    std::vector<std::byte> edid(128, std::byte{0});
    const std::array<std::uint8_t, 8> header = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        edid[i] = std::byte{header[i]};
    }
    // "JRY": J=10, R=18, Y=25 -> 0b0'01010'10010'11001
    const std::uint16_t manufacturer = (10 << 10) | (18 << 5) | 25;
    edid[8] = std::byte(manufacturer >> 8);
    edid[9] = std::byte(manufacturer & 0xFF);
    edid[10] = std::byte{0x80};
    edid[11] = std::byte{0x23};
    for (int i = 0; i < 4; ++i)
    {
        edid[12 + static_cast<std::size_t>(i)] = std::byte((serial >> (8 * i)) & 0xFF);
    }
    // Descriptor at 54: monitor name; at 72: serial text.
    const auto descriptor = [&](std::size_t offset, std::uint8_t tag, std::string_view text) {
        edid[offset + 3] = std::byte{tag};
        for (std::size_t i = 0; i < 13; ++i)
        {
            edid[offset + 5 + i] = std::byte(i < text.size() ? static_cast<std::uint8_t>(text[i]) : '\n');
        }
    };
    descriptor(54, 0xFC, "PHL 27M1");
    if (!serialText.empty())
    {
        descriptor(72, 0xFF, serialText);
    }
    return edid;
}

TEST(MonitorIdentity, FollowsTheLevelsOfAdr0004)
{
    using rmw::platform::monitorIdentityKey;
    using rmw::platform::parseEdid;
    constexpr std::string_view path =
        R"(\\?\DISPLAY#JRY2380#5&2e362cd1&0&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})";

    const auto unique = parseEdid(edidSample(0x1234ABCD, "SN 42"));
    EXPECT_EQ(unique.manufacturer, "JRY");
    EXPECT_EQ(unique.product, 0x2380);
    EXPECT_EQ(unique.name, "PHL 27M1");
    EXPECT_EQ(monitorIdentityKey(unique, path, 256, true), "JRY2380-1234ABCD-SN_42");

    // Generic serial (the host in ADR-0004) or twins: model + connector instance.
    const auto generic = parseEdid(edidSample(1, ""));
    EXPECT_EQ(monitorIdentityKey(generic, path, 256, true), "JRY2380-UID256");
    EXPECT_EQ(monitorIdentityKey(unique, path, 256, false), "JRY2380-UID256");
    // Without a device path (seen on real hardware) the QueryDisplayConfig ids give the same key.
    EXPECT_EQ(monitorIdentityKey(rmw::platform::edidFromIds(0x592A, 0x2380), "", 256, true),
              "JRY2380-UID256");

    // No EDID at all (Windows Sandbox): the device path, sanitized.
    const auto none = parseEdid({});
    EXPECT_TRUE(none.manufacturer.empty());
    const std::string key = monitorIdentityKey(none, R"(\\?\DISPLAY#Default_Monitor#1&2#{g})", 1, true);
    EXPECT_EQ(monitorIdentityKey(none, "", 7, true), "display-UID7");
    EXPECT_TRUE(key.starts_with("path-"));
    EXPECT_TRUE(rmw::domain::MonitorSelector::parse("edid:" + key).has_value()); // usable in a rice
}

TEST(Win32DisplayTopology, ListsTheMonitorsOfThisMachine)
{
    rmw::platform::Win32DisplayTopology topology;
    const auto monitors = topology.monitors();
    ASSERT_TRUE(monitors.has_value()) << monitors.error().toString();
    if (monitors->empty())
    {
        GTEST_SKIP() << "no active display";
    }
    EXPECT_EQ(std::ranges::count_if(*monitors, [](const auto& m) { return m.primary; }), 1);
    for (const auto& monitor : *monitors)
    {
        EXPECT_FALSE(monitor.identityKey.empty());
        EXPECT_FALSE(monitor.displayName.empty());
        EXPECT_GT(monitor.width(), 0);
        // The device path may be empty (QueryDisplayConfig does not always report it); the key
        // must still be usable in a rice.
        EXPECT_TRUE(rmw::domain::MonitorSelector::parse("edid:" + monitor.identityKey).has_value())
            << monitor.identityKey;
    }
}

TEST(ManagedTheme, ReplacesOnlyTheManagedKeys)
{
    const std::wstring base =
        L"; Copyright\r\n[Theme]\r\nDisplayName=@%SystemRoot%\\System32\\themeui.dll,-2013\r\n"
        L"ThemeId={1}\r\n\r\n[Control Panel\\Desktop]\r\nWallpaper=C:\\w.jpg\r\n\r\n"
        L"[VisualStyles]\r\nPath=%SystemRoot%\\aero.msstyles\r\nsystemmode=Light\r\n"
        L"AppMode=Light\r\nColorizationColor=0XC40078D7\r\n";
    const std::wstring theme = buildManagedTheme(base, ThemeLook{false, true, 0xC47AA2F7});
    EXPECT_NE(theme.find(L"DisplayName=Ricing Manager\r\n"), std::wstring::npos);
    EXPECT_NE(theme.find(L"SystemMode=Light\r\n"), std::wstring::npos);
    EXPECT_NE(theme.find(L"AppMode=Dark\r\n"), std::wstring::npos);
    EXPECT_NE(theme.find(L"ColorizationColor=0XC47AA2F7\r\n"), std::wstring::npos);
    EXPECT_NE(theme.find(L"Wallpaper=C:\\w.jpg\r\n"), std::wstring::npos); // the user's stays
    EXPECT_NE(theme.find(L"ThemeId={1}\r\n"), std::wstring::npos);
    EXPECT_EQ(theme.find(L"themeui.dll"), std::wstring::npos);
    EXPECT_EQ(theme.find(L"systemmode=Light"), std::wstring::npos); // replaced, not duplicated
}

TEST(ManagedTheme, AddsMissingKeysAndSections)
{
    const std::wstring withSections =
        L"[Theme]\nThemeId={1}\n\n[VisualStyles]\nPath=x\n\n[Sounds]\nSchemeName=a\n";
    const std::wstring theme = buildManagedTheme(withSections, ThemeLook{true, false, 0xC4000000});
    const auto visual = theme.find(L"[VisualStyles]");
    const auto sounds = theme.find(L"[Sounds]");
    const auto mode = theme.find(L"SystemMode=Dark");
    ASSERT_NE(mode, std::wstring::npos);
    EXPECT_GT(mode, visual);
    EXPECT_LT(mode, sounds); // inside its own section
    EXPECT_LT(theme.find(L"DisplayName=Ricing Manager"), visual);

    const std::wstring bare = buildManagedTheme(L"[Sounds]\nSchemeName=a", ThemeLook{});
    EXPECT_NE(bare.find(L"[Theme]\r\nDisplayName=Ricing Manager"), std::wstring::npos);
    EXPECT_NE(
        bare.find(L"[VisualStyles]\r\nSystemMode=Light\r\nAppMode=Light\r\nColorizationColor=0XC40078D4"),
        std::wstring::npos);

    // The other turn's file has its own name (the two must never be the same theme to Windows).
    const std::wstring second = buildManagedTheme(L"[Theme]\nDisplayName=Ricing Manager\n", ThemeLook{},
                                                  rmw::platform::kManagedThemeNameB);
    EXPECT_NE(second.find(L"DisplayName=Ricing Manager (2)"), std::wstring::npos);
    EXPECT_EQ(second.find(L"DisplayName=Ricing Manager\r\n"), std::wstring::npos);
}

} // namespace
