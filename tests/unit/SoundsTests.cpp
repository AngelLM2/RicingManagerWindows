#include "FakePorts.hpp"
#include "TempDir.hpp"

#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/domain/SoundFiles.hpp"
#include "rmw/engine/ContentStore.hpp"
#include "rmw/modules/Export.hpp"
#include "rmw/modules/Sounds.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using namespace rmw::domain;
using rmw::engine::Json;
using rmw::modules::SoundsLogic;
using rmw::modules::SoundsSystem;
using rmw::test::FakeRegistry;
using rmw::test::FakeShell;

void put32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
    }
}

void put16(std::vector<std::byte>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>(value >> 8U));
}

void tag(std::vector<std::byte>& out, const char* text)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>(text[i]));
    }
}

// A small, valid .wav (silence).
std::vector<std::byte> wave(std::uint16_t encoding = 1, std::uint16_t channels = 1, std::uint32_t rate = 8000,
                            std::uint16_t bits = 16, std::uint32_t samples = 64)
{
    const std::uint32_t blockAlign = channels * std::max<std::uint32_t>(bits / 8U, 1U);
    const std::uint32_t data = samples * blockAlign;
    std::vector<std::byte> out;
    tag(out, "RIFF");
    put32(out, 4 + (8 + 16) + (8 + data));
    tag(out, "WAVE");
    tag(out, "fmt ");
    put32(out, 16);
    put16(out, encoding);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * blockAlign);
    put16(out, static_cast<std::uint16_t>(blockAlign));
    put16(out, bits);
    tag(out, "data");
    put32(out, data);
    out.resize(out.size() + data);
    return out;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

TEST(SoundFiles, AcceptsWhatWindowsPlays)
{
    const auto pcm = validateWaveFile(wave());
    ASSERT_TRUE(pcm.has_value()) << pcm.error().toString();
    EXPECT_EQ(pcm->channels, 1);
    EXPECT_EQ(pcm->sampleRate, 8000u);
    EXPECT_EQ(pcm->dataBytes, 128u);
    EXPECT_TRUE(validateWaveFile(wave(3, 2, 48000, 32)).has_value()); // float stereo
    EXPECT_TRUE(validateWaveFile(wave(0xFFFE, 6, 96000, 24)).has_value());

    // A LIST chunk between "fmt " and "data", with an odd size (padded): still fine.
    auto listed = wave();
    std::vector<std::byte> list;
    tag(list, "LIST");
    put32(list, 3);
    list.resize(list.size() + 4); // 3 bytes and the pad
    listed.insert(listed.begin() + 36, list.begin(), list.end());
    const auto riff = static_cast<std::uint32_t>(listed.size() - 8);
    std::memcpy(listed.data() + 4, &riff, 4);
    EXPECT_TRUE(validateWaveFile(listed).has_value());
}

TEST(SoundFiles, RefusesWhatIsNotAPlayableWave)
{
    EXPECT_FALSE(validateWaveFile({}).has_value());
    auto notRiff = wave();
    notRiff[0] = std::byte{'X'};
    EXPECT_FALSE(validateWaveFile(notRiff).has_value());
    auto notWave = wave();
    notWave[8] = std::byte{'A'};
    EXPECT_FALSE(validateWaveFile(notWave).has_value());
    auto truncated = wave();
    truncated.resize(truncated.size() - 40); // the RIFF size says more
    EXPECT_FALSE(validateWaveFile(truncated).has_value());
    EXPECT_FALSE(validateWaveFile(wave(0x55)).has_value());            // MP3 inside: not for Windows' events
    EXPECT_FALSE(validateWaveFile(wave(1, 0)).has_value());            // no channels
    EXPECT_FALSE(validateWaveFile(wave(1, 1, 1000)).has_value());      // 1 kHz
    EXPECT_FALSE(validateWaveFile(wave(1, 1, 8000, 16, 0)).has_value()); // no samples

    // A chunk that claims to go past the end.
    auto lying = wave();
    const std::uint32_t huge = 0x7FFFFFF0;
    std::memcpy(lying.data() + 40, &huge, 4);
    EXPECT_FALSE(validateWaveFile(lying).has_value());

    std::vector<std::byte> big(rmw::limits::kSoundMaxBytes + 1);
    EXPECT_FALSE(validateWaveFile(big).has_value());
}

TEST(SoundsRice, ReadsWritesAndRefuses)
{
    Diagnostics diagnostics;
    const auto rice = parseRice(R"toml(
schema = 1
[meta]
name = "Quiet"

[sounds]
scheme = "none"

[sounds.events]
notification = "assets/sounds/pop.wav"
unlock = ""
)toml",
                                diagnostics);
    ASSERT_TRUE(rice.has_value()) << diagnostics.errorSummary("rice.toml");
    ASSERT_TRUE(rice->sounds.has_value());
    EXPECT_EQ(rice->sounds->scheme, SoundScheme::None);
    ASSERT_EQ(rice->sounds->events.size(), 2u);
    const auto again = parseRice(writeRice(*rice), diagnostics);
    ASSERT_TRUE(again.has_value()) << writeRice(*rice);
    ASSERT_TRUE(again->sounds.has_value());
    EXPECT_EQ(again->sounds->events.size(), 2u);
    EXPECT_EQ(again->sounds->scheme, SoundScheme::None);

    for (const char* bad : {
             "[sounds.events]\nbeeep = \"assets/a.wav\"\n",    // no such event
             "[sounds.events]\nbeep = \"assets/a.mp3\"\n",     // not a .wav
             "[sounds.events]\nbeep = \"../a.wav\"\n",         // outside the rice
             "[sounds]\nscheme = \"loud\"\n",
         })
    {
        Diagnostics d;
        EXPECT_FALSE(parseRice(std::string("schema = 1\n[meta]\nname = \"x\"\n") + bad, d).has_value()) << bad;
    }
}

class SoundsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        writeBytes(package() / "assets" / "sounds" / "pop.wav", wave());
        m_rice.meta.name = "Sounds";
    }

    std::filesystem::path package() const { return m_dir.path() / "package"; }

    rmw::Result<Json> desired()
    {
        rmw::engine::StoreAssetSink sink(m_store, package());
        return SoundsLogic().desiredFromRice(m_rice, sink);
    }

    rmw::test::TempDir m_dir;
    rmw::engine::ContentStore m_store{m_dir.path() / "store"};
    Rice m_rice;
};

TEST_F(SoundsTest, EventsAreValidatedAndStored)
{
    m_rice.sounds = SoundsConfig{std::nullopt, {{SoundEvent::Notification, "assets/sounds/pop.wav"},
                                                {SoundEvent::Unlock, ""}}};
    const auto fields = desired();
    ASSERT_TRUE(fields.has_value()) << fields.error().toString();
    const std::filesystem::path stored = (*fields)["notification"]["data"].get<std::string>();
    EXPECT_EQ(stored.parent_path(), m_dir.path() / "store");
    EXPECT_EQ(stored.extension(), ".wav");
    EXPECT_EQ((*fields)["unlock"], (Json{{"type", "string"}, {"data", ""}}));
    EXPECT_FALSE(fields->contains("beep")); // not in the rice: left alone
    EXPECT_EQ((*fields)["scheme"]["data"], ".Current");

    // Silence everywhere: Windows' "No Sounds" (every other event too).
    m_rice.sounds = SoundsConfig{SoundScheme::None, {}};
    const auto none = desired();
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->size(), kSoundEventCount + 2);
    EXPECT_EQ((*none)["scheme"]["data"], ".None");
    EXPECT_EQ((*none)["navigating"]["data"], "");
    EXPECT_EQ((*none)["others"], "silent");

    // A broken .wav never reaches the store.
    auto broken = wave();
    broken.resize(30);
    writeBytes(package() / "assets" / "sounds" / "broken.wav", broken);
    m_rice.sounds = SoundsConfig{std::nullopt, {{SoundEvent::Beep, "assets/sounds/broken.wav"}}};
    const auto refused = desired();
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("broken.wav"), std::string::npos);
}

TEST(SoundsSystem, CapturesAndAppliesTheEvents)
{
    const std::string asterisk = R"(AppEvents\Schemes\Apps\.Default\SystemAsterisk\.Current)";
    const std::string recycle = R"(AppEvents\Schemes\Apps\Explorer\EmptyRecycleBin\.Current)";
    FakeRegistry registry;
    registry.values[{asterisk, ""}] =
        rmw::ports::RegistryValue::fromExpandString(R"(%SystemRoot%\media\Windows Background.wav)");
    registry.values[{std::string(R"(AppEvents\Schemes)"), ""}] = rmw::ports::RegistryValue::fromString(".Default");
    FakeShell shell;
    SoundsSystem system(registry, shell);

    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->state["asterisk"]["keep"], true); // Windows' own: never copied
    EXPECT_EQ(before->state["empty-recycle-bin"], nullptr);
    EXPECT_EQ(before->state["scheme"]["data"], ".Default");

    ASSERT_TRUE(system.apply({{"empty-recycle-bin", {{"type", "expand"}, {"data", R"(C:\store\x.wav)"}}},
                              {"asterisk", {{"type", "string"}, {"data", ""}}},
                              {"scheme", {{"type", "string"}, {"data", ".Current"}}}}));
    EXPECT_EQ(registry.values.at({recycle, ""}), rmw::ports::RegistryValue::fromExpandString(R"(C:\store\x.wav)"));
    EXPECT_EQ(registry.values.at({asterisk, ""}).text, "");
    // Back to "absent".
    ASSERT_TRUE(system.apply({{"empty-recycle-bin", nullptr}}));
    EXPECT_FALSE(registry.values.contains({recycle, ""}));
    EXPECT_FALSE(system.apply({{"beep", 3}}).has_value()); // not the field shape
}

// The fourth real test: a rice's "no sounds" left every event but the eighteen playing (an app's own,
// Explorer's others). Now all of them, and a restore puts each back.
TEST(SoundsSystem, NoSoundsSilencesEveryEventAndARestoreBringsEachBack)
{
    const std::string app = R"(AppEvents\Schemes\Apps\SomeApp\Ding\.Current)";
    const std::string other = R"(AppEvents\Schemes\Apps\Explorer\BlockedPopup\.Current)";
    FakeRegistry registry;
    registry.values[{app, ""}] = rmw::ports::RegistryValue::fromString(R"(C:\Users\me\ding.wav)");
    registry.values[{other, ""}] = rmw::ports::RegistryValue::fromExpandString(R"(%SystemRoot%\media\Windows Pop-up Blocked.wav)");
    FakeShell shell;
    SoundsSystem system(registry, shell);
    const auto before = system.capture();
    ASSERT_TRUE(before.has_value());
    const Json others = before->state["others"];
    ASSERT_EQ(others.size(), 2u);
    EXPECT_EQ(others[R"(Apps\SomeApp\Ding)"]["data"], R"(C:\Users\me\ding.wav)");

    ASSERT_TRUE(system.apply({{"others", "silent"}}));
    EXPECT_EQ(registry.values.at({app, ""}).text, "");
    EXPECT_EQ(registry.values.at({other, ""}).text, "");
    const SoundsLogic logic;
    EXPECT_TRUE(logic.fieldEquivalent("others", "silent", system.capture()->state["others"]));
    EXPECT_FALSE(logic.fieldEquivalent("others", "silent", others));

    ASSERT_TRUE(system.apply({{"others", others}}));
    EXPECT_EQ(registry.values.at({app, ""}).text, R"(C:\Users\me\ding.wav)");
    EXPECT_EQ(registry.values.at({other, ""}).type, rmw::ports::RegistryValue::Type::ExpandString);

    // An event an app adds afterwards is no change of ours.
    Json later = system.capture()->state["others"];
    later[R"(Apps\NewApp\Hello)"] = Json{{"type", "string"}, {"data", R"(C:\new.wav)"}};
    EXPECT_TRUE(logic.fieldEquivalent("others", others, later));
}

TEST(SoundsExport, UsersFilesGoWindowsOwnStay)
{
    rmw::modules::CapturedLook look;
    look.sounds = Json{{"notification", {{"type", "expand"}, {"data", R"(C:\Users\me\pop.wav)"}}},
                       {"asterisk", {{"type", "expand"}, {"data", R"(%SystemRoot%\media\a.wav)"}, {"keep", true}}},
                       {"beep", {{"type", "string"}, {"data", ""}}},
                       {"scheme", {{"type", "string"}, {"data", ".Current"}}}};
    const auto exported = rmw::modules::exportRice("Mine", look, [](std::string_view) { return std::string("wav"); });
    ASSERT_TRUE(exported.has_value());
    ASSERT_EQ(exported->files.size(), 1u);
    EXPECT_EQ(exported->files[0].relative, "assets/sounds/notification.wav");
    Diagnostics diagnostics;
    const auto rice = parseRice(exported->toml, diagnostics);
    ASSERT_TRUE(rice.has_value()) << exported->toml;
    ASSERT_TRUE(rice->sounds.has_value());
    EXPECT_EQ(rice->sounds->events.size(), 1u);

    // Every event silent and the scheme "No Sounds": scheme = "none".
    Json silent = Json::object();
    for (std::size_t i = 0; i < kSoundEventCount; ++i)
    {
        silent[std::string(riceName(static_cast<SoundEvent>(i)))] = {{"type", "string"}, {"data", ""}};
    }
    silent["scheme"] = {{"type", "string"}, {"data", ".None"}};
    look.sounds = silent;
    const auto quiet = rmw::modules::exportRice("Quiet", look, [](std::string_view) { return std::string(); });
    ASSERT_TRUE(quiet.has_value());
    EXPECT_NE(quiet->toml.find("scheme = \"none\""), std::string::npos) << quiet->toml;
}

} // namespace
