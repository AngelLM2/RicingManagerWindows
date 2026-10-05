#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/engine/ModuleCatalog.hpp"
#include "rmw/engine/RiceEngine.hpp"
#include "rmw/modules/Tweaks.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace
{

using namespace rmw;
using engine::Json;

constexpr std::string_view kAdvanced = R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced)";
constexpr std::string_view kDesktop = R"(Control Panel\Desktop)";

domain::Tweak dword(std::string_view key, std::string_view name, std::uint64_t data)
{
    return {std::string(key), std::string(name), domain::RegistryType::Dword, data, false};
}

domain::Tweak text(std::string_view key, std::string_view name, std::string data)
{
    return {std::string(key), std::string(name), domain::RegistryType::String, std::move(data), false};
}

class TweaksModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto state =
            engine::StateDb::open(m_dir.path() / "state.db", {.synchronous = storage::Synchronous::Off});
        ASSERT_TRUE(state.has_value()) << state.error().toString();
        m_state.emplace(std::move(*state));
        m_store.emplace(m_dir.path() / "store");
        ASSERT_TRUE(m_catalog.add(m_logic));
        ASSERT_TRUE(m_catalog.finalize());
        m_runner.add(m_system);
        m_engine.emplace(*m_state, *m_store, m_catalog, m_runner);
        std::filesystem::create_directories(m_dir.path() / "rice");
        m_rice.meta.name = "Tweaked";
    }

    Result<engine::OperationReport> apply(std::vector<domain::Tweak> tweaks)
    {
        m_rice.tweaks = std::move(tweaks);
        return m_engine->apply(m_rice, m_dir.path() / "rice");
    }

    test::TempDir m_dir;
    test::FakeRegistry m_registry;
    test::FakeShell m_shell;
    modules::TweaksLogic m_logic;
    modules::TweaksSystem m_system{m_registry, m_shell};
    engine::ModuleCatalog m_catalog;
    engine::LocalModuleRunner m_runner;
    std::optional<engine::StateDb> m_state;
    std::optional<engine::ContentStore> m_store;
    std::optional<engine::RiceEngine> m_engine;
    domain::Rice m_rice;
};

TEST_F(TweaksModuleTest, AppliesAndRestoresTheOriginalValues)
{
    m_registry.setDword(kAdvanced, "HideFileExt", 1);
    ASSERT_TRUE(apply({dword(kAdvanced, "HideFileExt", 0), dword(kAdvanced, "ShowSecondsInSystemClock", 1),
                       text(kDesktop, "MenuShowDelay", "100")}));
    EXPECT_EQ(m_registry.dword(kAdvanced, "HideFileExt"), 0u);
    EXPECT_EQ(m_registry.dword(kAdvanced, "ShowSecondsInSystemClock"), 1u);
    EXPECT_FALSE(m_shell.broadcasts.empty());

    const auto restored = m_engine->restoreOriginal();
    ASSERT_TRUE(restored.has_value()) << restored.error().toString();
    EXPECT_EQ(m_registry.dword(kAdvanced, "HideFileExt"), 1u);
    // It did not exist before: it is removed again, not set to a guess.
    EXPECT_FALSE(m_registry.dword(kAdvanced, "ShowSecondsInSystemClock").has_value());
    EXPECT_FALSE(m_registry.values.contains({std::string(kDesktop), "MenuShowDelay"}));
}

TEST_F(TweaksModuleTest, ATweakAddedLaterIsRestoredToo)
{
    m_registry.setDword(kAdvanced, "HideFileExt", 1);
    m_registry.setDword(kAdvanced, "Hidden", 2);
    ASSERT_TRUE(apply({dword(kAdvanced, "HideFileExt", 0)}));
    // A later rice brings a tweak the first apply did not know about.
    ASSERT_TRUE(apply({dword(kAdvanced, "HideFileExt", 0), dword(kAdvanced, "Hidden", 1)}));
    EXPECT_EQ(m_registry.dword(kAdvanced, "Hidden"), 1u);
    ASSERT_TRUE(m_engine->restoreOriginal());
    EXPECT_EQ(m_registry.dword(kAdvanced, "HideFileExt"), 1u);
    EXPECT_EQ(m_registry.dword(kAdvanced, "Hidden"), 2u);
}

TEST_F(TweaksModuleTest, OnlyAllowListedValuesReachTheRegistry)
{
    // The system half checks again: a field outside the allow-list never gets written...
    const auto written = m_system.apply(Json{
        {R"(Software\Microsoft\Windows\CurrentVersion\Run|Evil)", Json{{"type", "string"}, {"data", "x"}}}});
    EXPECT_FALSE(written.has_value());
    const auto owned =
        m_system.apply(Json{{std::string(kAdvanced) + "|TaskbarAl", Json{{"type", "dword"}, {"data", 0}}}});
    EXPECT_FALSE(owned.has_value());
    EXPECT_TRUE(m_registry.values.empty());
    // ...nor read.
    const std::vector<std::string> fields{R"(Environment|Path)"};
    const auto captured = m_system.captureFields(fields);
    ASSERT_TRUE(captured.has_value());
    EXPECT_TRUE(captured->state.empty());
    // And data that names a program stops the apply before anything changes.
    EXPECT_FALSE(apply({text(kDesktop, "MenuShowDelay", R"(C:\x\evil.exe)")}).has_value());
    EXPECT_TRUE(m_registry.values.empty());
}

TEST_F(TweaksModuleTest, FieldsRoundTrip)
{
    const auto field = modules::tweakField(kAdvanced, "HideFileExt");
    const auto parsed = modules::parseTweakField(field);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->key, kAdvanced);
    EXPECT_EQ(parsed->value, "HideFileExt");
    EXPECT_FALSE(modules::parseTweakField("no separator").has_value());
}

} // namespace
