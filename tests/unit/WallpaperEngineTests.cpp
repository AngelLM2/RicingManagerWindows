#include "rmw/settings/WallpaperEngine.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using namespace rmw;

class WallpaperEngine : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_root = fs::temp_directory_path() /
                 fs::path(std::format(L"rmw-wallpaper-engine-{}", GetCurrentProcessId())) /
                 fs::path(::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(m_root);
        fs::create_directories(m_root);
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    fs::path write(const fs::path& relative, std::string_view bytes) const
    {
        const fs::path file = m_root / relative;
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file;
    }

    fs::path m_root;
};

TEST_F(WallpaperEngine, ReadsSteamsLibraryList)
{
    constexpr std::string_view vdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"431960"		"123"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
	}
	"2"
	{
		"PATH"		"D:\\SteamLibrary"
	}
	"3"
	{
		"path"		"relative\\nope"
	}
})";
    const auto libraries = settings::steamLibraries(vdf);
    ASSERT_EQ(libraries.size(), 2u);
    EXPECT_EQ(libraries[0], fs::path(LR"(C:\Program Files (x86)\Steam)"));
    EXPECT_EQ(libraries[1], fs::path(LR"(D:\SteamLibrary)"));
    EXPECT_TRUE(settings::steamLibraries("").empty());
    EXPECT_TRUE(settings::steamLibraries("\"path\"").empty()); // cut short
}

TEST_F(WallpaperEngine, ReadsProjectsAndKeepsTheirFilesInside)
{
    write(L"1001/project.json",
          "\xEF\xBB\xBF{\"title\":\"Sakura night\",\"type\":\"Video\",\"file\":\"night.mp4\",\"preview\":\"preview.gif\","
          "\"workshopid\":\"1001\"}");
    write(L"1001/night.mp4", "....ftyp");
    write(L"1001/preview.gif", "GIF89a");
    auto video = settings::readEngineProject(m_root / L"1001");
    ASSERT_TRUE(video) << video.error().message();
    EXPECT_EQ(video->title, "Sakura night");
    EXPECT_EQ(video->id, "1001");
    EXPECT_EQ(video->kind, settings::EngineKind::Video);
    EXPECT_EQ(video->video, m_root / L"1001" / L"night.mp4");
    EXPECT_EQ(video->preview, m_root / L"1001" / L"preview.gif");
    EXPECT_TRUE(video->usable());

    // A scene: only its picture. Its id may be a number.
    write(L"2002/project.json", R"({"title":"","type":"scene","file":"scene.json","preview":"preview.jpg","workshopid":2002})");
    write(L"2002/scene.json", "{}");
    write(L"2002/preview.jpg", "\xFF\xD8\xFF");
    auto scene = settings::readEngineProject(m_root / L"2002");
    ASSERT_TRUE(scene);
    EXPECT_EQ(scene->kind, settings::EngineKind::Scene);
    EXPECT_EQ(scene->title, "2002"); // no title: the folder's name
    EXPECT_EQ(scene->id, "2002");
    EXPECT_TRUE(scene->video.empty());
    EXPECT_FALSE(scene->preview.empty());

    // Names that lead out of the folder, or files that are not there, are left out.
    write(L"secret.mp4", "....ftyp");
    for (const std::string_view file : {R"(..\\secret.mp4)", R"(C:\\Windows\\win.ini)", R"(\\secret.mp4)", "gone.mp4"})
    {
        write(L"3003/project.json",
              std::format(R"({{"title":"x","type":"video","file":"{}","preview":"../secret.mp4"}})", file));
        auto escaped = settings::readEngineProject(m_root / L"3003");
        ASSERT_TRUE(escaped) << file;
        EXPECT_TRUE(escaped->video.empty()) << file;
        EXPECT_TRUE(escaped->preview.empty()) << file;
        EXPECT_FALSE(escaped->usable()) << file;
    }
    // A video we cannot play is not a video.
    write(L"4004/project.json", R"({"title":"x","type":"video","file":"clip.avi"})");
    write(L"4004/clip.avi", "RIFF");
    auto avi = settings::readEngineProject(m_root / L"4004");
    ASSERT_TRUE(avi);
    EXPECT_TRUE(avi->video.empty());

    // Broken or missing project.json: an error, not a crash.
    write(L"5005/project.json", "{\"title\":");
    EXPECT_FALSE(settings::readEngineProject(m_root / L"5005"));
    write(L"6006/project.json", "[1,2]");
    EXPECT_FALSE(settings::readEngineProject(m_root / L"6006"));
    EXPECT_FALSE(settings::readEngineProject(m_root / L"7007"));
}

TEST_F(WallpaperEngine, FindsWhatTheUserPointsAt)
{
    write(L"mine/b/project.json", R"({"title":"beta","type":"video","file":"b.mp4"})");
    write(L"mine/b/b.mp4", "....ftyp");
    write(L"mine/a/project.json", R"({"title":"Alpha","type":"web","file":"index.html","preview":"p.png"})");
    write(L"mine/a/p.png", "\x89PNG");
    write(L"mine/broken/project.json", "nope");
    write(L"single/project.json", R"({"title":"Gamma","type":"scene","preview":"p.jpg"})");
    write(L"single/p.jpg", "\xFF\xD8\xFF");
    const std::array<fs::path, 3> extra = {m_root / L"mine", m_root / L"single", m_root / L"mine" / L"a"};
    const auto found = settings::findEngineWallpapers(extra);
    // Steam may have its own on this computer: look only at ours.
    std::vector<std::string> titles;
    for (const auto& wallpaper : found)
    {
        if (wallpaper.folder.native().starts_with(m_root.native()))
        {
            titles.push_back(wallpaper.title);
        }
    }
    EXPECT_EQ(titles, (std::vector<std::string>{"Alpha", "beta", "Gamma"})); // sorted, "a" once
    const auto limited = settings::findEngineWallpapers(extra, 1);
    EXPECT_EQ(limited.size(), 1u);
}

} // namespace
