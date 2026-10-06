#include "CursorSamples.hpp"
#include "TempDir.hpp"

#include "rmw/package/Package.hpp"

#include <gtest/gtest.h>
#include <zip.h>

#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using rmw::package::Kind;

std::vector<std::byte> bytesOf(std::string_view text)
{
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

// A PNG header declaring width x height (the reader never decodes further).
std::vector<std::byte> png(std::uint32_t width = 1, std::uint32_t height = 1)
{
    rmw::test::Writer w;
    w.u8(0x89).tag("PNG\r\n\x1a\n").u32(0x0D000000).tag("IHDR");
    const auto big = [&w](std::uint32_t v) {
        w.u8(static_cast<std::uint8_t>(v >> 24)).u8(static_cast<std::uint8_t>(v >> 16));
        w.u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v));
    };
    big(width);
    big(height);
    w.u8(8).u8(6).u8(0).u8(0).u8(0).u32(0);
    return w.bytes;
}

std::vector<std::byte> mp4(std::size_t size = 4096)
{
    rmw::test::Writer w;
    w.u8(0).u8(0).u8(0).u8(24).tag("ftypisom").zeros(16);
    w.zeros(size > w.bytes.size() ? size - w.bytes.size() : 0);
    return w.bytes;
}

std::uint32_t crc32(std::span<const std::byte> bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : bytes)
    {
        crc ^= std::to_integer<std::uint32_t>(b);
        for (int k = 0; k < 8; ++k)
        {
            crc = (crc & 1U) != 0 ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
    }
    return ~crc;
}

// A zip written byte by byte (stored), so a test can make it as wrong as it likes.
struct RawEntry
{
    std::string name;
    std::vector<std::byte> data;
    std::uint16_t flags = 0x0800; // UTF-8 names
    std::uint16_t method = 0;
    std::uint16_t madeBy = 20; // MS-DOS
    std::uint32_t external = 0;
    std::optional<std::uint32_t> crc = std::nullopt;
};

std::vector<std::byte> rawZip(const std::vector<RawEntry>& entries)
{
    rmw::test::Writer out;
    rmw::test::Writer directory;
    for (const auto& e : entries)
    {
        const auto offset = static_cast<std::uint32_t>(out.bytes.size());
        const std::uint32_t crc = e.crc.value_or(crc32(e.data));
        const auto size = static_cast<std::uint32_t>(e.data.size());
        const auto nameSize = static_cast<std::uint16_t>(e.name.size());
        out.u32(0x04034b50).u16(20).u16(e.flags).u16(e.method).u16(0).u16(0x21).u32(crc).u32(size).u32(size);
        out.u16(nameSize).u16(0).tag(e.name).append(e.data);
        directory.u32(0x02014b50).u16(e.madeBy).u16(20).u16(e.flags).u16(e.method).u16(0).u16(0x21);
        directory.u32(crc).u32(size).u32(size).u16(nameSize).u16(0).u16(0).u16(0).u16(0);
        directory.u32(e.external).u32(offset).tag(e.name);
    }
    const auto start = static_cast<std::uint32_t>(out.bytes.size());
    const auto count = static_cast<std::uint16_t>(entries.size());
    out.append(directory.bytes);
    out.u32(0x06054b50).u16(0).u16(0).u16(count).u16(count);
    out.u32(static_cast<std::uint32_t>(directory.bytes.size())).u32(start).u16(0);
    return out.bytes;
}

RawEntry manifest(std::string name = "rice.toml")
{
    return {std::move(name), bytesOf("[rice]\nname = \"Test\"\n")};
}

class PackageTest : public ::testing::Test
{
protected:
    fs::path staging() const
    {
        const fs::path folder = m_dir.path() / "staging";
        fs::create_directories(folder);
        return folder;
    }

    fs::path write(const fs::path& relative, std::span<const std::byte> bytes) const
    {
        const fs::path file = m_dir.path() / relative;
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return file;
    }

    static std::vector<std::byte> read(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto* data = reinterpret_cast<const std::byte*>(chars.data());
        return {data, data + chars.size()};
    }

    static std::size_t filesIn(const fs::path& folder)
    {
        std::size_t count = 0;
        for (const auto& item : fs::recursive_directory_iterator(folder))
        {
            count += item.is_regular_file() ? 1 : 0;
        }
        return count;
    }

    rmw::test::TempDir m_dir;
};

TEST_F(PackageTest, PackedRiceComesBackTheSame)
{
    const fs::path rice = m_dir.path() / "My rice";
    write("My rice/rice.toml", bytesOf("[rice]\nname = \"Mine\"\n"));
    write("My rice/assets/wall.png", png(1920, 1080));
    write("My rice/assets/loop.mp4", mp4(200'000));
    write("My rice/cursors/arrow.cur", rmw::test::cursorFile());
    write("My rice/LICENSE", bytesOf("CC BY 4.0\n"));
    write("My rice/.vscode/settings.json", bytesOf("{}"));
    write("My rice/Thumbs.db", bytesOf("Explorer's"));

    const fs::path archive = m_dir.path() / "mine.rice";
    const auto size = rmw::package::pack(rice, Kind::Rice, archive);
    ASSERT_TRUE(size) << size.error().toString();
    EXPECT_EQ(*size, fs::file_size(archive));

    const auto imported = rmw::package::extract(archive, Kind::Rice, staging());
    ASSERT_TRUE(imported) << imported.error().toString();
    EXPECT_EQ(imported->files, 5u);
    EXPECT_EQ(imported->stripped, "");
    for (const char* file :
         {"rice.toml", "assets/wall.png", "assets/loop.mp4", "cursors/arrow.cur", "LICENSE"})
    {
        EXPECT_EQ(read(staging() / file), read(rice / file)) << file;
    }
    EXPECT_FALSE(fs::exists(staging() / ".vscode"));
    EXPECT_FALSE(fs::exists(staging() / "Thumbs.db"));

    // The same files make the same package.
    const fs::path again = m_dir.path() / "again.rice";
    ASSERT_TRUE(rmw::package::pack(rice, Kind::Rice, again));
    EXPECT_EQ(read(again), read(archive));
    EXPECT_FALSE(fs::exists(m_dir.path() / "again.rice.partial"));

    // What an install would refuse, packing refuses (and leaves the last package as it was).
    write("My rice/tool.exe", bytesOf("MZ"));
    const auto refused = rmw::package::pack(rice, Kind::Rice, again);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message().find("tool.exe"), std::string::npos);
    EXPECT_EQ(read(again), read(archive));
    fs::remove(rice / "tool.exe");
    write("My rice/assets/broken.png", bytesOf("not a png"));
    EXPECT_FALSE(rmw::package::pack(rice, Kind::Rice, again));
}

TEST_F(PackageTest, CheckOnlyWritesNothing)
{
    const auto zip = rawZip({manifest(), {"a.png", png()}});
    const auto imported = rmw::package::extract(zip, Kind::Rice, {});
    ASSERT_TRUE(imported) << imported.error().toString();
    EXPECT_EQ(imported->files, 2u);
}

TEST_F(PackageTest, OneTopFolderIsLeftOut)
{
    const auto zip = rawZip({{"Sakura/", {}}, manifest("Sakura/rice.toml"), {"Sakura/assets/a.png", png()}});
    const auto imported = rmw::package::extract(zip, Kind::Rice, staging());
    ASSERT_TRUE(imported) << imported.error().toString();
    EXPECT_EQ(imported->stripped, "Sakura");
    EXPECT_TRUE(fs::exists(staging() / "rice.toml"));
    EXPECT_TRUE(fs::exists(staging() / "assets" / "a.png"));
}

TEST_F(PackageTest, ManifestMustBeThere)
{
    const auto zip = rawZip({{"a.png", png()}, {"deep/rice.toml", manifest().data}});
    const auto imported = rmw::package::extract(zip, Kind::Rice, staging());
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code(), rmw::ErrorCode::NotFound);
}

class BadName : public PackageTest, public ::testing::WithParamInterface<const char*>
{
};

TEST_P(BadName, IsRefusedBeforeAnythingIsWritten)
{
    // The good file comes first: nothing is written from a package with one bad name.
    const auto zip = rawZip({manifest(), {"good.png", png()}, {GetParam(), png()}});
    const auto imported = rmw::package::extract(zip, Kind::Rice, staging());
    EXPECT_FALSE(imported) << GetParam();
    EXPECT_EQ(filesIn(staging()), 0u);
}

INSTANTIATE_TEST_SUITE_P(Package, BadName,
                         ::testing::Values("../evil.png", "a/../../evil.png", "a\\..\\..\\evil.png",
                                           "/abs.png", "C:/Windows/evil.png", "C:evil.png",
                                           "\\\\server\\share\\evil.png", "\\\\?\\C:\\evil.png",
                                           "a.png:hidden", "CON.png", "lpt1/a.png", "PROGRA~1/a.png",
                                           "dot./a.png", "space /a.png", "a//b.png", "./a.png", "tab\t.png",
                                           "COM\xC2\xB9.png", "bad\xFF.png"));

TEST_F(PackageTest, SameNameInAnotherCaseIsRefused)
{
    const auto zip = rawZip({manifest(), {"assets/a.png", png()}, {"ASSETS/A.PNG", png()}});
    const auto imported = rmw::package::extract(zip, Kind::Rice, staging());
    ASSERT_FALSE(imported);
    EXPECT_NE(imported.error().message().find("twice"), std::string::npos);
}

TEST_F(PackageTest, LinksEncryptionAndOtherMethodsAreRefused)
{
    RawEntry link{"a.png", bytesOf("rice.toml")};
    link.madeBy = (3U << 8U) | 20U; // Unix
    link.external = 0120777U << 16U;
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), link}), Kind::Rice, staging()));

    RawEntry encrypted{"a.png", png()};
    encrypted.flags |= 1U;
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), encrypted}), Kind::Rice, staging()));

    RawEntry bzip2{"a.png", png()};
    bzip2.method = 12;
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), bzip2}), Kind::Rice, staging()));
    EXPECT_EQ(filesIn(staging()), 0u);
}

TEST_F(PackageTest, OnlyTheKindsOwnFilesGoIn)
{
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), {"run.exe", bytesOf("MZ")}}), Kind::Rice, {}));
    EXPECT_FALSE(
        rmw::package::extract(rawZip({manifest(), {"main.luau", bytesOf("return 1")}}), Kind::Rice, {}));
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), {"nested.zip", bytesOf("PK")}}), Kind::Rice, {}));
    EXPECT_TRUE(rmw::package::extract(rawZip({manifest("plugin.toml"), {"main.luau", bytesOf("return 1")}}),
                                      Kind::Plugin, {}));
    EXPECT_FALSE(
        rmw::package::extract(rawZip({manifest("plugin.toml"), {"loop.mp4", mp4()}}), Kind::Plugin, {}));
}

TEST_F(PackageTest, ContentMustMatchTheName)
{
    const auto refused = [](RawEntry entry) {
        return !rmw::package::extract(rawZip({manifest(), std::move(entry)}), Kind::Rice, {});
    };
    const std::vector<std::byte> jpeg = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE0}};
    EXPECT_TRUE(refused({"a.png", jpeg}));
    EXPECT_TRUE(refused({"a.png", png(20'000, 20'000)})); // a huge image in a few bytes
    EXPECT_TRUE(refused({"a.png", png(10'000, 10'000)})); // 100 MP
    EXPECT_FALSE(refused({"a.png", png(16'384, 1'000)}));
    EXPECT_TRUE(refused({"a.cur", bytesOf("not a cursor at all")}));
    EXPECT_TRUE(refused({"a.ani", rmw::test::cursorFile()}));
    EXPECT_TRUE(refused({"a.mp4", bytesOf(std::string(100, 'x'))}));
    EXPECT_TRUE(refused({"notes.md", bytesOf("bad \xFF utf-8")}));
    EXPECT_TRUE(refused({"rice2.toml", std::vector<std::byte>((64U << 10U) + 1, std::byte{'#'})}));
}

TEST_F(PackageTest, DamagedDataIsRefused)
{
    RawEntry broken{"a.png", png()};
    broken.crc = 0x12345678;
    EXPECT_FALSE(rmw::package::extract(rawZip({manifest(), broken}), Kind::Rice, staging()));
    EXPECT_FALSE(rmw::package::extract(bytesOf("PK\x03\x04 not really a zip"), Kind::Rice, {}));
    EXPECT_FALSE(rmw::package::extract(std::span<const std::byte>(), Kind::Rice, {}));
}

TEST_F(PackageTest, ZipBombsStopAtTheRealBytes)
{
    // 40 MB of zeros deflate to a few dozen KB: far past 100:1.
    std::vector<std::byte> zeros = mp4(40U << 20U);
    const fs::path archive = m_dir.path() / "bomb.rice";
    {
        int error = 0;
        zip_t* zip = zip_open(archive.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        ASSERT_NE(zip, nullptr);
        const std::string toml = "[rice]\nname = \"Bomb\"\n";
        zip_source_t* text = zip_source_buffer(zip, toml.data(), toml.size(), 0);
        ASSERT_GE(zip_file_add(zip, "rice.toml", text, ZIP_FL_ENC_UTF_8), 0);
        zip_source_t* video = zip_source_buffer(zip, zeros.data(), zeros.size(), 0);
        const zip_int64_t index = zip_file_add(zip, "loop.mp4", video, ZIP_FL_ENC_UTF_8);
        ASSERT_GE(index, 0);
        ASSERT_EQ(zip_set_file_compression(zip, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 9), 0);
        ASSERT_EQ(zip_close(zip), 0);
    }
    ASSERT_LT(fs::file_size(archive), 1U << 20U);
    const auto imported = rmw::package::extract(archive, Kind::Rice, staging());
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code(), rmw::ErrorCode::LimitExceeded) << imported.error().toString();
}

TEST_F(PackageTest, LinksInTheStagingFolderCannotLeadOut)
{
    fs::create_directories(m_dir.path() / "outside");
    const std::string command =
        std::format(R"(mklink /J "{}" "{}" >nul 2>nul)", (staging() / "assets").string(),
                    (m_dir.path() / "outside").string());
    if (std::system(command.c_str()) != 0)
    {
        GTEST_SKIP() << "cannot create a junction here";
    }
    const auto imported =
        rmw::package::extract(rawZip({manifest(), {"assets/a.png", png()}}), Kind::Rice, staging());
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code(), rmw::ErrorCode::AccessDenied);
    EXPECT_FALSE(fs::exists(m_dir.path() / "outside" / "a.png"));
}

TEST_F(PackageTest, PackingRefusesLinks)
{
    write("Linked/rice.toml", manifest().data);
    fs::create_directories(m_dir.path() / "elsewhere");
    const std::string command =
        std::format(R"(mklink /J "{}" "{}" >nul 2>nul)", (m_dir.path() / "Linked" / "assets").string(),
                    (m_dir.path() / "elsewhere").string());
    if (std::system(command.c_str()) != 0)
    {
        GTEST_SKIP() << "cannot create a junction here";
    }
    EXPECT_FALSE(rmw::package::pack(m_dir.path() / "Linked", Kind::Rice, m_dir.path() / "linked.rice"));
}

} // namespace
