#include "rmw/base/Utf.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/domain/RiceWriter.hpp"
#include "rmw/domain/Settings.hpp"
#include "rmw/machine/MachineIcons.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

using namespace rmw::machine;
using rmw::domain::MachineIcon;

constexpr std::string_view kHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

// What CommandLineToArgvW makes of a line like ours (quotes around the paths, no quote inside).
std::vector<std::wstring> split(const std::wstring& line)
{
    std::vector<std::wstring> args;
    std::wstring current;
    bool quoted = false;
    bool any = false;
    for (const wchar_t c : line)
    {
        if (c == L'"')
        {
            quoted = !quoted;
            any = true;
        }
        else if (c == L' ' && !quoted)
        {
            if (any)
            {
                args.push_back(current);
            }
            current.clear();
            any = false;
        }
        else
        {
            current += c;
            any = true;
        }
    }
    if (any)
    {
        args.push_back(current);
    }
    return args;
}

TEST(MachineIcons, TheCommandLineGoesThereAndBack)
{
    const Command set{Command::Kind::SetShellIcons,
                      {{MachineIcon::Folder, std::string(kHash), LR"(C:\Users\me's PC\rices\a b\assets\folder.ico)"},
                       {MachineIcon::Drive, std::string(64, 'b'), LR"(C:\r\drive.ico)"}}};
    const auto line = commandLine(set);
    ASSERT_TRUE(line.has_value());
    const auto parsed = parseCommand(split(*line));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().toString();
    EXPECT_EQ(*parsed, set);

    const Command restore{Command::Kind::RestoreDefaults, {}};
    EXPECT_EQ(parseCommand(split(*commandLine(restore))).value(), restore);

    EXPECT_EQ(shellIconValues(MachineIcon::Folder).size(), 2u); // closed and open
    EXPECT_EQ(shellIconValues(MachineIcon::Drive)[0], L"8");
}

TEST(MachineIcons, OnlyTheClosedListGoesThrough)
{
    const std::vector<std::vector<std::wstring>> bad = {
        {},
        {L"format-c"},
        {L"restore-machine-defaults", L"extra"},
        {L"set-shell-icons"},
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a')},                  // no file
        {L"set-shell-icons", L"--wallpaper", std::wstring(64, L'a'), L"C:\\a.ico"}, // not an icon of ours
        {L"set-shell-icons", L"--folder", L"ABC", L"C:\\a.ico"},                     // not a hash
        {L"set-shell-icons", L"--folder", std::wstring(64, L'A'), L"C:\\a.ico"},     // upper case
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a'), L"a.ico"},         // relative
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a'), L"\\\\server\\a.ico"},
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a'), L"\\\\?\\C:\\a.ico"},
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a'), L"C:\\a\"b.ico"},
        {L"set-shell-icons", L"--folder", std::wstring(64, L'a'), L"C:\\a.ico", L"--folder",
         std::wstring(64, L'a'), L"C:\\b.ico"}, // twice
    };
    for (const auto& args : bad)
    {
        EXPECT_FALSE(parseCommand(args).has_value()) << (args.empty() ? std::string("(none)") : rmw::utf16To8Lossy(args[0]));
    }
    EXPECT_FALSE(commandLine({Command::Kind::SetShellIcons, {{MachineIcon::Folder, "x", L"C:\\a.ico"}}}).has_value());
}

TEST(MachineIcons, TheJournalKeepsWhatThereWas)
{
    const Journal journal{true, {{L"3", false, LR"(C:\Windows\x.dll,3)"}, {L"8", true, LR"(%SystemRoot%\y.ico)"}}};
    const auto back = parseJournal(toJson(journal));
    ASSERT_TRUE(back.has_value()) << back.error().toString();
    EXPECT_EQ(*back, journal);
    EXPECT_FALSE(parseJournal(R"({"version":1,"values":[{"name":"Run","type":"string","data":"x"}]})").has_value());
    EXPECT_FALSE(parseJournal("{}").has_value());
    EXPECT_EQ(storedName(kHash), rmw::utf8To16Lossy(std::string(kHash)) + L".ico");
}

TEST(MachineIcons, TheRiceAndTheSetting)
{
    rmw::domain::Diagnostics diagnostics;
    const auto rice = rmw::domain::parseRice(
        "schema = 1\n[meta]\nname = \"m\"\n[icons.machine]\nfolder = \"assets/f.ico\"\ndrive = \"assets/d.ico\"\n",
        diagnostics);
    ASSERT_TRUE(rice.has_value()) << diagnostics.errorSummary("rice.toml");
    ASSERT_EQ(rice->icons->machine.size(), 2u);
    const auto again = rmw::domain::parseRice(rmw::domain::writeRice(*rice), diagnostics);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->icons->machine, rice->icons->machine);
    rmw::domain::Diagnostics d;
    EXPECT_FALSE(
        rmw::domain::parseRice("schema = 1\n[meta]\nname = \"m\"\n[icons.machine]\nfolder = \"assets/f.png\"\n", d)
            .has_value());

    rmw::domain::Diagnostics s;
    const auto settings = rmw::domain::parseSettings("schema = 1\n[icons]\nmachine = true\n", s, {});
    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(settings->machineIcons);
    const auto written = rmw::domain::parseSettings(rmw::domain::writeSettings(*settings), s, {});
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(written->machineIcons);
}

} // namespace
