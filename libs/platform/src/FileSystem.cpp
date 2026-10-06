#include "rmw/platform/FileSystem.hpp"

#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace rmw::platform
{
namespace
{

constexpr DWORD kChunkBytes = 64 * 1024;

Error ntError(NTSTATUS status, std::string_view context)
{
    return {ErrorCode::System,
            std::format("{} failed (NTSTATUS 0x{:08X})", context, static_cast<std::uint32_t>(status)),
            status};
}

bool succeeded(NTSTATUS status) noexcept
{
    return status >= 0;
}

std::wstring stripVerbatimPrefix(std::wstring path)
{
    if (path.starts_with(LR"(\\?\UNC\)"))
    {
        return LR"(\\)" + path.substr(8);
    }
    if (path.starts_with(LR"(\\?\)"))
    {
        return path.substr(4);
    }
    return path;
}

} // namespace

Result<std::filesystem::path> finalPathOfHandle(HANDLE handle)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const DWORD length =
            GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                      FILE_NAME_NORMALIZED /* | VOLUME_NAME_DOS, also 0 */);
        if (length == 0)
        {
            return std::unexpected(lastError("GetFinalPathNameByHandle"));
        }
        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(stripVerbatimPrefix(std::move(buffer)));
        }
        buffer.resize(length); // too small: `length` includes the terminator
    }
    return fail(ErrorCode::System, "GetFinalPathNameByHandle kept growing");
}

Result<void> writeAll(HANDLE file, std::span<const std::byte> bytes)
{
    while (!bytes.empty())
    {
        const DWORD chunk = bytes.size() > kChunkBytes ? kChunkBytes : static_cast<DWORD>(bytes.size());
        DWORD written = 0;
        if (WriteFile(file, bytes.data(), chunk, &written, nullptr) == FALSE || written == 0)
        {
            return std::unexpected(lastError("WriteFile"));
        }
        bytes = bytes.subspan(written);
    }
    return {};
}

// ---- SHA-256 ------------------------------------------------------------------------------------

Sha256::Sha256(void* handle) noexcept
    : m_hash(handle)
{
}

Result<Sha256> Sha256::create()
{
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
    if (!succeeded(status))
    {
        return std::unexpected(ntError(status, "BCryptCreateHash"));
    }
    return Sha256(hash);
}

Sha256::~Sha256()
{
    if (m_hash != nullptr)
    {
        BCryptDestroyHash(m_hash);
    }
}

Sha256::Sha256(Sha256&& other) noexcept
    : m_hash(std::exchange(other.m_hash, nullptr))
{
}

Sha256& Sha256::operator=(Sha256&& other) noexcept
{
    if (this != &other)
    {
        if (m_hash != nullptr)
        {
            BCryptDestroyHash(m_hash);
        }
        m_hash = std::exchange(other.m_hash, nullptr);
    }
    return *this;
}

Result<void> Sha256::update(std::span<const std::byte> bytes)
{
    if (m_hash == nullptr)
    {
        return fail(ErrorCode::InvalidArgument, "hash already finished");
    }
    while (!bytes.empty())
    {
        const ULONG chunk = bytes.size() > kChunkBytes ? kChunkBytes : static_cast<ULONG>(bytes.size());
        // BCryptHashData takes a non-const pointer but does not modify the input.
        auto* data = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()));
        if (const NTSTATUS status = BCryptHashData(m_hash, data, chunk, 0); !succeeded(status))
        {
            return std::unexpected(ntError(status, "BCryptHashData"));
        }
        bytes = bytes.subspan(chunk);
    }
    return {};
}

Result<Sha256Digest> Sha256::finish()
{
    if (m_hash == nullptr)
    {
        return fail(ErrorCode::InvalidArgument, "hash already finished");
    }
    Sha256Digest digest{};
    const NTSTATUS status = BCryptFinishHash(m_hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(std::exchange(m_hash, nullptr));
    if (!succeeded(status))
    {
        return std::unexpected(ntError(status, "BCryptFinishHash"));
    }
    return digest;
}

Result<Sha256Digest> sha256(std::span<const std::byte> bytes)
{
    auto hash = Sha256::create();
    if (!hash)
    {
        return std::unexpected(std::move(hash.error()));
    }
    if (auto updated = hash->update(bytes); !updated)
    {
        return std::unexpected(std::move(updated.error()));
    }
    return hash->finish();
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes)
    {
        text += digits[byte >> 4];
        text += digits[byte & 0x0F];
    }
    return text;
}

Result<std::string> randomHex(std::size_t byteCount)
{
    std::vector<std::uint8_t> bytes(byteCount);
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!succeeded(status))
    {
        return std::unexpected(ntError(status, "BCryptGenRandom"));
    }
    return toHex(bytes);
}

// ---- files ----------------------------------------------------------------------------------

Result<HashedCopy> copyHashed(const std::filesystem::path& source, const std::filesystem::path& target,
                              std::uint64_t maxBytes)
{
    const wil::unique_hfile in(
        CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in)
    {
        return std::unexpected(lastError("open source file"));
    }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(in.get(), FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE)
    {
        return std::unexpected(lastError("read source attributes"));
    }
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        return fail(ErrorCode::InvalidData, "not a regular file (links and folders are refused)");
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(in.get(), &size) == FALSE)
    {
        return std::unexpected(lastError("read source size"));
    }
    if (std::cmp_greater(size.QuadPart, maxBytes))
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("file is too large ({} bytes, limit {})", size.QuadPart, maxBytes));
    }

    auto sourceFinal = finalPathOfHandle(in.get());
    if (!sourceFinal)
    {
        return std::unexpected(std::move(sourceFinal.error()));
    }
    auto hash = Sha256::create();
    if (!hash)
    {
        return std::unexpected(std::move(hash.error()));
    }

    wil::unique_hfile out(
        CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out)
    {
        return std::unexpected(lastError("create target file"));
    }
    ScopeExit removeTarget([&]() noexcept {
        out.reset();
        DeleteFileW(target.c_str());
    });

    std::vector<std::byte> buffer(kChunkBytes);
    std::uint64_t total = 0;
    while (true)
    {
        DWORD read = 0;
        if (ReadFile(in.get(), buffer.data(), kChunkBytes, &read, nullptr) == FALSE)
        {
            return std::unexpected(lastError("read source file"));
        }
        if (read == 0)
        {
            break;
        }
        total += read;
        if (total > maxBytes)
        {
            return fail(ErrorCode::LimitExceeded, "file grew past the size limit while being copied");
        }
        const std::span<const std::byte> chunk(buffer.data(), read);
        if (auto hashed = hash->update(chunk); !hashed)
        {
            return std::unexpected(std::move(hashed.error()));
        }
        if (auto written = writeAll(out.get(), chunk); !written)
        {
            return std::unexpected(std::move(written.error()));
        }
    }
    if (FlushFileBuffers(out.get()) == FALSE)
    {
        return std::unexpected(lastError("flush target file"));
    }
    auto digest = hash->finish();
    if (!digest)
    {
        return std::unexpected(std::move(digest.error()));
    }
    removeTarget.release();
    return HashedCopy{toHex(*digest), total, std::move(*sourceFinal)};
}

Result<std::vector<std::byte>> readFileLimited(const std::filesystem::path& path, std::uint64_t maxBytes)
{
    // FILE_SHARE_DELETE: a writer replacing the file (writeFileAtomic) while it is read must not fail (a
    // tiler reading its plan made the core's new plan get lost).
    const wil::unique_hfile file(
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
    {
        return std::unexpected(lastError("open file"));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE)
    {
        return std::unexpected(lastError("read file attributes"));
    }
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        return fail(ErrorCode::InvalidData, "not a regular file (links and folders are refused)");
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE)
    {
        return std::unexpected(lastError("read file size"));
    }
    if (std::cmp_greater(size.QuadPart, maxBytes))
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("file is too large ({} bytes, limit {})", size.QuadPart, maxBytes));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < bytes.size())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, kChunkBytes));
        DWORD read = 0;
        if (ReadFile(file.get(), bytes.data() + total, chunk, &read, nullptr) == FALSE)
        {
            return std::unexpected(lastError("read file"));
        }
        if (read == 0)
        {
            bytes.resize(total); // shrank while reading
            break;
        }
        total += read;
    }
    return bytes;
}

Result<void> replaceFile(const std::filesystem::path& from, const std::filesystem::path& to)
{
    const std::wstring& destination = to.native();
    const wil::unique_hfile file(CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
                                             FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file)
    {
        const std::size_t nameBytes = destination.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, destination.data(), nameBytes);
        if (SetFileInformationByHandle(file.get(), FileRenameInfoEx, info,
                                       static_cast<DWORD>(storage.size())) != FALSE)
        {
            return {};
        }
    }
    // FAT/exFAT and older file systems: plain replace (still atomic for the directory entry).
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        return std::unexpected(lastError("replace file"));
    }
    return {};
}

Result<void> writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> bytes,
                             Durability durability)
{
    auto suffix = randomHex(8);
    if (!suffix)
    {
        return std::unexpected(std::move(suffix.error()));
    }
    std::filesystem::path temporary = path;
    temporary += L".tmp-";
    temporary += std::filesystem::path(*suffix);

    {
        const wil::unique_hfile file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
        {
            return std::unexpected(lastError("create temporary file"));
        }
        auto written = writeAll(file.get(), bytes);
        if (written && durability == Durability::Flushed && FlushFileBuffers(file.get()) == FALSE)
        {
            written = std::unexpected(lastError("flush temporary file"));
        }
        if (!written)
        {
            DeleteFileW(temporary.c_str());
            return written;
        }
    }
    if (auto replaced = replaceFile(temporary, path); !replaced)
    {
        DeleteFileW(temporary.c_str());
        return replaced;
    }
    return {};
}

Result<std::filesystem::path> expandEnvironment(std::string_view text)
{
    auto wide = utf8To16(text);
    if (!wide)
    {
        return std::unexpected(std::move(wide.error()));
    }
    const DWORD needed = ExpandEnvironmentStringsW(wide->c_str(), nullptr, 0);
    if (needed == 0 || needed > 32'768)
    {
        return fail(ErrorCode::InvalidData, "cannot expand the path");
    }
    std::wstring expanded(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(wide->c_str(), expanded.data(), needed);
    if (written == 0 || written > needed)
    {
        return fail(ErrorCode::InvalidData, "cannot expand the path");
    }
    expanded.resize(written - 1);
    return std::filesystem::path(std::move(expanded));
}

std::string sniffFileType(const std::filesystem::path& file)
{
    const wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle)
    {
        return {};
    }
    std::array<unsigned char, 12> magic{};
    DWORD read = 0;
    if (ReadFile(handle.get(), magic.data(), static_cast<DWORD>(magic.size()), &read, nullptr) == FALSE ||
        read < 4)
    {
        return {};
    }
    const auto starts = [&](std::initializer_list<unsigned char> bytes) {
        return std::equal(bytes.begin(), bytes.end(), magic.begin());
    };
    if (starts({0xFF, 0xD8, 0xFF}))
    {
        return "jpg";
    }
    if (starts({0x89, 'P', 'N', 'G'}))
    {
        return "png";
    }
    if (starts({'B', 'M'}))
    {
        return "bmp";
    }
    if (starts({'G', 'I', 'F'}))
    {
        return "gif";
    }
    if (starts({0x00, 0x00, 0x02, 0x00}))
    {
        return "cur";
    }
    if (starts({0x00, 0x00, 0x01, 0x00}))
    {
        return "ico";
    }
    if (read >= 12 && starts({'R', 'I', 'F', 'F'}) && magic[8] == 'W' && magic[9] == 'A' && magic[10] == 'V' &&
        magic[11] == 'E')
    {
        return "wav";
    }
    if (read >= 12 && starts({'R', 'I', 'F', 'F'}) && magic[8] == 'A' && magic[9] == 'C' &&
        magic[10] == 'O' && magic[11] == 'N')
    {
        return "ani";
    }
    return {};
}

Result<std::filesystem::path> finalPath(const std::filesystem::path& path)
{
    const wil::unique_hfile handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle)
    {
        return std::unexpected(lastError("open path"));
    }
    return finalPathOfHandle(handle.get());
}

bool isWithin(const std::filesystem::path& child, const std::filesystem::path& root)
{
    std::wstring rootText = root.native();
    while (rootText.size() > 3 && (rootText.back() == L'\\' || rootText.back() == L'/'))
    {
        rootText.pop_back();
    }
    const std::wstring& childText = child.native();
    if (childText.size() < rootText.size())
    {
        return false;
    }
    if (CompareStringOrdinal(childText.data(), static_cast<int>(rootText.size()), rootText.data(),
                             static_cast<int>(rootText.size()), TRUE) != CSTR_EQUAL)
    {
        return false;
    }
    return childText.size() == rootText.size() || childText[rootText.size()] == L'\\' ||
           rootText.back() == L'\\';
}

} // namespace rmw::platform
