#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/engine/RiceEngine.hpp"
#include "rmw/modules/Appearance.hpp"
#include "rmw/modules/Taskbar.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace
{

using namespace rmw::modules;
using rmw::engine::Json;
using rmw::test::FakeRegistry;
using rmw::test::FakeShell;
using rmw::test::FakeThemeApi;

constexpr std::string_view kPersonalize = R"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
constexpr std::string_view kDwm = R"(Software\Microsoft\Windows\DWM)";
constexpr std::string_view kAdvanced = R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)";
constexpr std::string_view kSearch = R"(Software\Microsoft\Windows\CurrentVersion\Search)";

class NoAssets final : public rmw::engine::IAssetSink
{
public:
    rmw::Result<std::filesystem::path> fromPackage(std::string_view) override
    {
        return rmw::fail(rmw::ErrorCode::Unsupported, "no assets");
    }
    rmw::Result<std::vector<std::byte>> readPackageFile(std::string_view, std::uint64_t) override
    {
        return rmw::fail(rmw::ErrorCode::Unsupported, "no assets");
    }
    rmw::Result<std::filesystem::path> fromSystem(const std::filesystem::path&, std::string_view) override
    {
        return rmw::fail(rmw::ErrorCode::Unsupported, "no assets");
    }
};

rmw::domain::TimeOfDay at(int hour, int minute = 0)
{
    return {static_cast<std::uint8_t>(hour), static_cast<std::uint8_t>(minute)};
}

// ---- appearance ---------------------------------------------------------------------------------

TEST(Appearance, RiceBecomesFields)
{
    rmw::domain::Rice rice;
    rice.appearance = rmw::domain::AppearanceConfig{};
    rice.appearance->mode = rmw::domain::ThemeMode::Dark;
    rice.appearance->accent = rmw::domain::Color{0x7a, 0xa2, 0xf7, 0xff};
    rice.appearance->accentOnTaskbar = true;
    NoAssets assets;
    const auto desired = AppearanceLogic().desiredFromRice(rice, assets);
    ASSERT_TRUE(desired.has_value());
    EXPECT_EQ(
        *desired,
        (Json{
            {"appsLight", false}, {"systemLight", false}, {"accent", "#7AA2F7"}, {"accentOnTaskbar", true}}));

    rice.appearance->mode = rmw::domain::ThemeMode::System;
    EXPECT_FALSE(AppearanceLogic().desiredFromRice(rice, assets)->contains("appsLight"));
    EXPECT_TRUE(AppearanceLogic().desiredFromRice(rmw::domain::Rice{}, assets)->empty());
}

TEST(Appearance, ScheduleFollowsTheClock)
{
    rmw::domain::Rice rice;
    rice.appearance = rmw::domain::AppearanceConfig{};
    rice.appearance->mode = rmw::domain::ThemeMode::Schedule;
    rice.appearance->schedule = rmw::domain::AppearanceSchedule{at(7), at(19)};
    NoAssets assets;
    EXPECT_EQ(AppearanceLogic([] { return at(12); }).desiredFromRice(rice, assets)->at("appsLight"), true);
    EXPECT_EQ(AppearanceLogic([] { return at(19); }).desiredFromRice(rice, assets)->at("appsLight"), false);
    EXPECT_EQ(AppearanceLogic([] { return at(6, 59); }).desiredFromRice(rice, assets)->at("appsLight"),
              false);

    // A light period that crosses midnight (night-shift workers).
    const rmw::domain::AppearanceSchedule night{at(22), at(6)};
    EXPECT_TRUE(isLightPeriod(night, at(23)));
    EXPECT_TRUE(isLightPeriod(night, at(2)));
    EXPECT_FALSE(isLightPeriod(night, at(12)));
}

TEST(Appearance, CaptureUsesWindowsDefaultsForMissingValues)
{
    FakeRegistry registry;
    FakeShell shell;
    FakeThemeApi theme(registry);
    AppearanceSystem system(registry, shell, theme);
    const auto captured = system.capture();
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->state, (Json{{"appsLight", true},
                                     {"systemLight", true},
                                     {"transparency", true},
                                     {"accentOnTaskbar", false},
                                     {"accent", "#0078D4"}}));
}

TEST(Appearance, ModeAndAccentGoThroughTheManagedTheme)
{
    FakeRegistry registry;
    registry.setDword(kDwm, "ColorizationColor", 0xC4112233);
    FakeShell shell;
    FakeThemeApi theme(registry);
    AppearanceSystem system(registry, shell, theme);

    ASSERT_TRUE(system.apply({{"systemLight", false}}));
    ASSERT_EQ(theme.applied.size(), 1u);
    // Fields not asked for keep their current value in the generated theme.
    EXPECT_EQ(theme.applied[0], (rmw::ports::ThemeLook{true, false, 0xC4112233}));

    ASSERT_TRUE(system.apply({{"accent", "#7aa2f7"}, {"transparency", false}}));
    EXPECT_EQ(theme.applied[1].colorizationArgb, 0xC47AA2F7u);
    EXPECT_EQ(registry.dword(kPersonalize, "EnableTransparency"), 0u);
    EXPECT_EQ(shell.broadcasts.back(), "ImmersiveColorSet");

    EXPECT_FALSE(system.apply({{"accent", "blue"}}).has_value());
    EXPECT_FALSE(system.apply({{"appsLight", 1}}).has_value());
}

TEST(Appearance, ThemeFailureFallsBackForModeButNotForAccent)
{
    FakeRegistry registry;
    FakeShell shell;
    FakeThemeApi theme(registry);
    theme.fail = true;
    AppearanceSystem system(registry, shell, theme);

    ASSERT_TRUE(system.apply({{"appsLight", false}, {"systemLight", false}}));
    EXPECT_EQ(registry.dword(kPersonalize, "AppsUseLightTheme"), 0u);
    EXPECT_EQ(shell.broadcasts.back(), "ImmersiveColorSet");

    const auto accent = system.apply({{"accent", "#FF0000"}});
    ASSERT_FALSE(accent.has_value());
    EXPECT_EQ(accent.error().code(), rmw::ErrorCode::Timeout);
}

TEST(Appearance, AccentComparisonIgnoresCase)
{
    const AppearanceLogic logic;
    EXPECT_TRUE(logic.fieldEquivalent("accent", "#7aa2f7", "#7AA2F7"));
    EXPECT_FALSE(logic.fieldEquivalent("accent", "#7aa2f7", "#7AA2F8"));
    EXPECT_FALSE(logic.fieldEquivalent("appsLight", true, false));
}

// ---- taskbar ------------------------------------------------------------------------------------

TEST(Taskbar, RiceBecomesRegistryValues)
{
    rmw::domain::Rice rice;
    rice.taskbar = rmw::domain::TaskbarConfig{rmw::domain::TaskbarAlignment::Left, true, false,
                                              rmw::domain::TaskbarSearch::Hidden};
    NoAssets assets;
    EXPECT_EQ(TaskbarLogic().desiredFromRice(rice, assets).value(),
              (Json{{"alignment", 0}, {"autohide", true}, {"taskView", 0}, {"search", 0}}));
}

TEST(Taskbar, AbsentValuesAreCapturedAsNullAndRestoredByDeleting)
{
    FakeRegistry registry;
    registry.setDword(kAdvanced, "TaskbarAl", 1);
    FakeShell shell;
    TaskbarSystem system(registry, shell);

    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->state,
              (Json{{"alignment", 1}, {"taskView", nullptr}, {"search", nullptr}, {"autohide", false}}));

    ASSERT_TRUE(system.apply({{"taskView", 0}, {"search", 3}, {"autohide", true}}));
    EXPECT_EQ(registry.dword(kAdvanced, "ShowTaskViewButton"), 0u);
    EXPECT_EQ(registry.dword(kSearch, "SearchboxTaskbarMode"), 3u);
    EXPECT_TRUE(shell.autohide);
    EXPECT_EQ(shell.broadcasts, (std::vector<std::string>{"TraySettings"}));

    ASSERT_TRUE(system.apply({{"taskView", nullptr}}));
    EXPECT_FALSE(registry.dword(kAdvanced, "ShowTaskViewButton").has_value());
    EXPECT_FALSE(system.apply({{"search", "box"}}).has_value());
    EXPECT_FALSE(system.apply({{"autohide", 1}}).has_value());
}

// ---- both modules under the real engine -------------------------------------------------------

TEST(ModulesContract, ApplyThenRestoreOriginalLeavesTheRegistryAsItWas)
{
    const rmw::test::TempDir dir;
    FakeRegistry registry;
    registry.setDword(kPersonalize, "AppsUseLightTheme", 1);
    registry.setDword(kAdvanced, "TaskbarAl", 1);
    FakeShell shell;
    FakeThemeApi theme(registry);
    const auto original = registry.values;

    const AppearanceLogic appearanceLogic;
    AppearanceSystem appearanceSystem(registry, shell, theme);
    const TaskbarLogic taskbarLogic;
    TaskbarSystem taskbarSystem(registry, shell);
    rmw::engine::ModuleCatalog catalog;
    ASSERT_TRUE(catalog.add(appearanceLogic));
    ASSERT_TRUE(catalog.add(taskbarLogic));
    ASSERT_TRUE(catalog.finalize());
    rmw::engine::LocalModuleRunner runner;
    runner.add(appearanceSystem);
    runner.add(taskbarSystem);

    auto state =
        rmw::engine::StateDb::open(dir.path() / "state.db", {.synchronous = rmw::storage::Synchronous::Off});
    ASSERT_TRUE(state.has_value());
    rmw::engine::ContentStore store(dir.path() / "store");
    rmw::engine::RiceEngine engine(*state, store, catalog, runner);

    rmw::domain::Rice rice;
    rice.meta.name = "Contract";
    rice.appearance = rmw::domain::AppearanceConfig{};
    rice.appearance->mode = rmw::domain::ThemeMode::Dark;
    rice.appearance->accent = rmw::domain::Color{0x7a, 0xa2, 0xf7, 0xff};
    rice.taskbar = rmw::domain::TaskbarConfig{rmw::domain::TaskbarAlignment::Left, std::nullopt, false,
                                              rmw::domain::TaskbarSearch::Icon};
    const auto applied = engine.apply(rice, dir.path());
    ASSERT_TRUE(applied.has_value()) << applied.error().toString();
    EXPECT_EQ(registry.dword(kPersonalize, "AppsUseLightTheme"), 0u);
    EXPECT_EQ(registry.dword(kAdvanced, "TaskbarAl"), 0u);
    EXPECT_EQ(registry.dword(kSearch, "SearchboxTaskbarMode"), 1u);

    const auto restored = engine.restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_TRUE(restored->skipped.empty());
    // The theme rewrote SystemUsesLightTheme/ColorizationColor that did not exist before; the
    // restore puts their values back (Windows defaults), the rest is exactly the original.
    EXPECT_EQ(registry.dword(kPersonalize, "AppsUseLightTheme"), 1u);
    EXPECT_EQ(registry.dword(kAdvanced, "TaskbarAl"), 1u);
    EXPECT_FALSE(registry.dword(kAdvanced, "ShowTaskViewButton").has_value());
    EXPECT_FALSE(registry.dword(kSearch, "SearchboxTaskbarMode").has_value());
    EXPECT_EQ(registry.dword(kDwm, "ColorizationColor"), 0xC40078D4u);
    for (const auto& [key, value] : original)
    {
        EXPECT_EQ(registry.values.at(key), value) << key.first << "\\" << key.second;
    }
}

} // namespace
