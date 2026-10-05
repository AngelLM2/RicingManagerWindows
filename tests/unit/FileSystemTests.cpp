#include "TempDir.hpp"

#include "rmw/platform/FileSystem.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace
{

using namespace rmw::platform;

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

TEST(Sha256, MatchesKnownVectors)
{
    EXPECT_EQ(toHex(sha256(bytesOf("abc")).value()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(toHex(sha256({}).value()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    auto incremental = Sha256::create();
    ASSERT_TRUE(incremental.has_value());
    ASSERT_TRUE(incremental->update(bytesOf("a")));
    ASSERT_TRUE(incremental->update(bytesOf("bc")));
    EXPECT_EQ(toHex(incremental->finish().value()), toHex(sha256(bytesOf("abc")).value()));
    EXPECT_FALSE(incremental->update(bytesOf("x")).has_value()); // finished
}

TEST(FileSystem, CopyHashedCopiesHashesAndResolvesThePath)
{
    const rmw::test::TempDir dir;
    writeText(dir.path() / "a.txt", "abc");
    const auto copy = copyHashed(dir.path() / "a.txt", dir.path() / "b.bin", 1024);
    ASSERT_TRUE(copy.has_value()) << copy.error().toString();
    EXPECT_EQ(copy->sha256Hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(copy->bytes, 3u);
    EXPECT_EQ(readText(dir.path() / "b.bin"), "abc");
    EXPECT_TRUE(isWithin(copy->sourceFinalPath, finalPath(dir.path()).value()));
}

TEST(FileSystem, CopyHashedRefusesOversizeExistingTargetsAndFolders)
{
    const rmw::test::TempDir dir;
    writeText(dir.path() / "a.txt", "abcdef");
    const auto tooBig = copyHashed(dir.path() / "a.txt", dir.path() / "b.bin", 5);
    ASSERT_FALSE(tooBig.has_value());
    EXPECT_EQ(tooBig.error().code(), rmw::ErrorCode::LimitExceeded);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "b.bin"));

    writeText(dir.path() / "taken.bin", "x");
    EXPECT_FALSE(copyHashed(dir.path() / "a.txt", dir.path() / "taken.bin", 1024).has_value());
    EXPECT_EQ(readText(dir.path() / "taken.bin"), "x");

    std::filesystem::create_directory(dir.path() / "folder");
    EXPECT_FALSE(copyHashed(dir.path() / "folder", dir.path() / "c.bin", 1024).has_value());
    EXPECT_FALSE(copyHashed(dir.path() / "missing", dir.path() / "d.bin", 1024).has_value());
}

TEST(FileSystem, WriteFileAtomicReplacesContent)
{
    const rmw::test::TempDir dir;
    const auto path = dir.path() / L"config é.toml";
    ASSERT_TRUE(writeFileAtomic(path, bytesOf("first")));
    ASSERT_TRUE(writeFileAtomic(path, bytesOf("second")));
    EXPECT_EQ(readText(path), "second");
    // No temporary files left behind.
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir.path()), {}), 1);
}

TEST(FileSystem, IsWithinComparesWholeComponents)
{
    EXPECT_TRUE(isWithin(L"C:\\Data\\Rices\\x\\a.png", L"C:\\data\\rices\\X"));
    EXPECT_TRUE(isWithin(L"C:\\Data\\Rices", L"C:\\Data\\Rices\\"));
    EXPECT_FALSE(isWithin(L"C:\\Data\\RicesEvil\\a.png", L"C:\\Data\\Rices"));
    EXPECT_FALSE(isWithin(L"C:\\Data", L"C:\\Data\\Rices"));
    EXPECT_TRUE(isWithin(L"C:\\a.png", L"C:\\"));
}

} // namespace
