#include "rmw/platform/FileSystem.hpp"
#include "rmw/platform/Win32Ports.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <string>

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
}

} // namespace
