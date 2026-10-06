#include "rmw/package/Package.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/ScopeExit.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/CursorFiles.hpp"
#include "rmw/domain/SoundFiles.hpp"
#include "rmw/domain/Images.hpp"
#include "rmw/domain/PathRules.hpp"
#include "rmw/domain/RiceParser.hpp"
#include "rmw/platform/FileSystem.hpp"

#include <windows.h>

#include <wil/resource.h>

#include <zip.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <utility>

namespace rmw::package
{
namespace
{

namespace fs = std::filesystem;

constexpr std::uint64_t kTextMaxBytes = 256u << 10;
constexpr std::uint64_t kScriptMaxBytes = 256u << 10;
constexpr std::uint64_t kSvgMaxBytes = 1u << 20;
constexpr std::uint64_t kJsonMaxBytes = 1u << 20;
constexpr std::uint64_t kRatioSlack = 1u << 20; // tiny files compress better than 100:1
constexpr std::size_t kChunk = std::size_t{64} * 1024;
constexpr std::size_t kVideoHead = 64;

std::string lowerExtension(std::string_view path)
{
    const auto slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    std::string extension(dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return extension;
}

std::string lowerName(std::string_view path)
{
    const auto slash = path.find_last_of('/');
    std::string name(slash == std::string_view::npos ? path : path.substr(slash + 1));
    std::ranges::transform(name, name.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return name;
}

// LICENSE, COPYING, NOTICE, AUTHORS (any case, with or without .txt/.md): texts every package may
// carry.
bool isNoticeFile(std::string_view path)
{
    std::string name = lowerName(path);
    if (const auto dot = name.find('.'); dot != std::string::npos)
    {
        name.resize(dot);
    }
    return name == "license" || name == "licence" || name == "copying" || name == "notice" ||
           name == "authors" || name == "credits";
}

bool isVideo(std::string_view extension)
{
    return extension == "mp4" || extension == "m4v" || extension == "webm";
}

bool isText(std::string_view extension)
{
    return extension == "toml" || extension == "luau" || extension == "slint" || extension == "json" ||
           extension == "md" || extension == "txt";
}

Result<void> bad(std::string_view path, std::string_view why)
{
    return fail(ErrorCode::InvalidData, std::format("{}: {}", path, why));
}

std::uint32_t le32(std::span<const std::byte> bytes, std::size_t at)
{
    return std::to_integer<std::uint32_t>(bytes[at]) | (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[at + 3]) << 24U);
}

bool startsWith(std::span<const std::byte> bytes, std::string_view signature)
{
    return bytes.size() >= signature.size() &&
           std::ranges::equal(bytes.first(signature.size()), signature, [](std::byte b, char c) {
               return std::to_integer<unsigned char>(b) == static_cast<unsigned char>(c);
           });
}

// The upper-case form Windows compares file names with (close enough to NTFS's own table).
std::wstring foldedName(std::string_view path)
{
    std::wstring wide = utf8To16Lossy(path);
    if (!wide.empty())
    {
        CharUpperBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    }
    return wide;
}

// A file of the staging folder, made new (never an existing one, never through a link) and checked
// to really be inside the folder.
class StagedFile
{
public:
    static Result<StagedFile> create(const fs::path& root, const fs::path& finalRoot,
                                     std::string_view relative)
    {
        const fs::path target = (root / fs::path(utf8To16Lossy(relative))).make_preferred();
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec)
        {
            return fail(ErrorCode::Io, std::format("{}: {}", relative, ec.message()));
        }
        const std::wstring longPath = L"\\\\?\\" + fs::absolute(target).native();
        wil::unique_hfile file(CreateFileW(longPath.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file)
        {
            return fail(ErrorCode::Io, std::format("{}: cannot create it ({})", relative, GetLastError()));
        }
        auto where = platform::finalPathOfHandle(file.get());
        if (!where || !platform::isWithin(*where, finalRoot))
        {
            // A folder on the way was a link: the empty file it led to goes away as it closes.
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(file.get(), FileDispositionInfo, &remove, sizeof(remove));
            return fail(ErrorCode::AccessDenied,
                        std::format("{}: not inside the package's folder", relative));
        }
        return StagedFile(std::move(file));
    }

    Result<void> write(std::span<const std::byte> bytes) { return platform::writeAll(m_file.get(), bytes); }

private:
    explicit StagedFile(wil::unique_hfile file)
        : m_file(std::move(file))
    {
    }

    wil::unique_hfile m_file;
};

struct ArchiveDeleter
{
    void operator()(zip_t* archive) const noexcept { zip_discard(archive); }
};
using Archive = std::unique_ptr<zip_t, ArchiveDeleter>;

struct FileDeleter
{
    void operator()(zip_file_t* file) const noexcept { zip_fclose(file); }
};
using ZipFile = std::unique_ptr<zip_file_t, FileDeleter>;

struct Entry
{
    zip_uint64_t index = 0;
    std::string path; // checked, '/'-separated, before the top folder is left out
    zip_uint64_t compressed = 0;
};

Result<Imported> extractFrom(zip_source_t* source, Kind kind, const fs::path& staging)
{
    zip_error_t error;
    zip_error_init(&error);
    const Archive archive(zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error));
    if (!archive)
    {
        zip_source_free(source);
        const std::string why = zip_error_strerror(&error);
        zip_error_fini(&error);
        return fail(ErrorCode::InvalidData, std::format("not a readable zip: {}", why));
    }
    zip_error_fini(&error);

    const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
    if (count < 0 || std::cmp_greater(count, limits::kPackageMaxEntries))
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("more than {} entries", limits::kPackageMaxEntries));
    }

    // Every name first: nothing is written from a package that has one bad name.
    std::vector<Entry> entries;
    std::set<std::wstring> seen;
    for (zip_uint64_t i = 0; std::cmp_less(i, count); ++i)
    {
        // The central directory's name, decoded as the zip says (CP437 unless marked UTF-8). libzip
        // takes a "Unicode Path" field only when it was written for these very bytes (its CRC);
        // either way the name checked here is the one written (ADR-0015).
        const char* raw = zip_get_name(archive.get(), i, ZIP_FL_ENC_STRICT);
        if (raw == nullptr)
        {
            return fail(ErrorCode::InvalidData, std::format("entry {} has no name", i));
        }
        std::string name(raw);
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive.get(), i, 0, &stat) != 0)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: unreadable entry", name));
        }
        zip_uint8_t system = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(archive.get(), i, 0, &system, &attributes) == 0 &&
            system == ZIP_OPSYS_UNIX && ((attributes >> 16U) & 0170000U) == 0120000U)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: links are not allowed", name));
        }
        if ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && stat.encryption_method != ZIP_EM_NONE)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: encrypted entries are not allowed", name));
        }
        if ((stat.valid & ZIP_STAT_COMP_METHOD) != 0 && stat.comp_method != ZIP_CM_STORE &&
            stat.comp_method != ZIP_CM_DEFLATE)
        {
            return fail(ErrorCode::Unsupported,
                        std::format("{}: only stored and deflated files are read", name));
        }
        const bool folder = name.ends_with('/') || name.ends_with('\\');
        if (folder)
        {
            name.pop_back();
            if (name.empty())
            {
                continue;
            }
        }
        auto checked = checkEntryName(name);
        if (!checked)
        {
            return std::unexpected(std::move(checked.error()));
        }
        if (folder)
        {
            continue; // folders come with their files
        }
        if (!allowedFile(kind, *checked))
        {
            return fail(ErrorCode::Unsupported,
                        std::format("{}: this kind of file has no place in a {}", *checked,
                                    kind == Kind::Rice ? "rice" : "plugin"));
        }
        if (!seen.insert(foldedName(*checked)).second)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: twice in the package", *checked));
        }
        entries.push_back(
            {i, std::move(*checked), (stat.valid & ZIP_STAT_COMP_SIZE) != 0 ? stat.comp_size : 0});
    }

    // One folder holding everything (and the manifest): left out, as if the files were at the top.
    Imported imported;
    const std::string manifest = kind == Kind::Rice ? "rice.toml" : "plugin.toml";
    const auto isManifest = [&](std::string_view path, std::size_t skip) {
        return path.size() > skip && path.find('/', skip) == std::string_view::npos &&
               lowerName(path) == manifest;
    };
    const bool atTop = std::ranges::any_of(entries, [&](const Entry& e) { return isManifest(e.path, 0); });
    if (!atTop && !entries.empty())
    {
        const std::string first = entries.front().path.substr(0, entries.front().path.find('/'));
        const std::string prefix = first + "/";
        const bool shared =
            std::ranges::all_of(entries, [&](const Entry& e) { return e.path.starts_with(prefix); });
        if (shared &&
            std::ranges::any_of(entries, [&](const Entry& e) { return isManifest(e.path, prefix.size()); }))
        {
            imported.stripped = first;
            for (auto& entry : entries)
            {
                entry.path.erase(0, prefix.size());
            }
        }
    }
    if (std::ranges::none_of(entries, [&](const Entry& e) { return isManifest(e.path, 0); }))
    {
        return fail(ErrorCode::NotFound, std::format("no {} at the top of the package", manifest));
    }

    fs::path finalRoot;
    if (!staging.empty())
    {
        auto resolved = platform::finalPath(staging);
        if (!resolved)
        {
            return std::unexpected(std::move(resolved.error()));
        }
        finalRoot = std::move(*resolved);
    }

    std::vector<std::byte> buffer(kChunk);
    for (const auto& entry : entries)
    {
        const std::uint64_t max = maxFileBytes(entry.path);
        const bool video = isVideo(lowerExtension(entry.path));
        ZipFile file(zip_fopen_index(archive.get(), entry.index, 0));
        if (!file)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: cannot be read", entry.path));
        }
        std::optional<StagedFile> out;
        if (!staging.empty() && video)
        {
            auto created = StagedFile::create(staging, finalRoot, entry.path);
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            out.emplace(std::move(*created));
        }
        std::vector<std::byte> whole; // small files: checked whole before they are written
        std::vector<std::byte> head;
        std::uint64_t real = 0;
        while (true)
        {
            const zip_int64_t read = zip_fread(file.get(), buffer.data(), buffer.size());
            if (read < 0)
            {
                return fail(ErrorCode::InvalidData,
                            std::format("{}: damaged ({})", entry.path,
                                        zip_error_strerror(zip_file_get_error(file.get()))));
            }
            if (read == 0)
            {
                break;
            }
            real += static_cast<std::uint64_t>(read);
            imported.bytes += static_cast<std::uint64_t>(read);
            // Real bytes, never the size the header claims.
            if (real > max)
            {
                return fail(ErrorCode::LimitExceeded,
                            std::format("{}: larger than {} MB", entry.path, max >> 20));
            }
            if (imported.bytes > limits::kPackageMaxTotalBytes)
            {
                return fail(ErrorCode::LimitExceeded, "the package is larger than 1 GB unpacked");
            }
            if (real > (std::max<zip_uint64_t>(entry.compressed, 1) * limits::kPackageMaxCompressionRatio) +
                           kRatioSlack)
            {
                return fail(ErrorCode::LimitExceeded,
                            std::format("{}: compressed more than 100:1", entry.path));
            }
            const std::span<const std::byte> chunk(buffer.data(), static_cast<std::size_t>(read));
            if (video)
            {
                // Its first bytes are checked before any reaches the disk (a shorter file: after).
                if (head.size() < kVideoHead)
                {
                    const auto take = std::min(chunk.size(), kVideoHead - head.size());
                    head.insert(head.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
                    if (head.size() == kVideoHead)
                    {
                        if (auto checked = checkContent(entry.path, head); !checked)
                        {
                            return std::unexpected(std::move(checked.error()));
                        }
                    }
                }
                if (out)
                {
                    if (auto written = out->write(chunk); !written)
                    {
                        return std::unexpected(std::move(written.error()).withContext(entry.path));
                    }
                }
            }
            else
            {
                whole.insert(whole.end(), chunk.begin(), chunk.end());
            }
        }
        // libzip checks the CRC as the file ends.
        if (zip_fclose(file.release()) != 0)
        {
            return fail(ErrorCode::InvalidData, std::format("{}: damaged (checksum)", entry.path));
        }
        if (auto checked = checkContent(entry.path, video ? std::span<const std::byte>(head) : whole);
            !checked)
        {
            return std::unexpected(std::move(checked.error()));
        }
        if (!staging.empty() && !video)
        {
            auto created = StagedFile::create(staging, finalRoot, entry.path);
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            if (auto written = created->write(whole); !written)
            {
                return std::unexpected(std::move(written.error()).withContext(entry.path));
            }
        }
        ++imported.files;
    }
    return imported;
}

// ---- writing a zip (stored) -----------------------------------------------------------------------

std::uint32_t crc32(std::uint32_t crc, std::span<const std::byte> bytes)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::byte b : bytes)
    {
        crc = table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

void put16(std::vector<std::byte>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void put32(std::vector<std::byte>& out, std::uint32_t value)
{
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

void putText(std::vector<std::byte>& out, std::string_view text)
{
    for (const char c : text)
    {
        out.push_back(static_cast<std::byte>(c));
    }
}

} // namespace

Result<std::string> checkEntryName(std::string_view name)
{
    if (name.empty() || name.find('\0') != std::string_view::npos)
    {
        return fail(ErrorCode::InvalidData, "an entry without a usable name");
    }
    auto checked = domain::validatePackagePath(name);
    if (!checked)
    {
        return fail(ErrorCode::InvalidData, std::format("{}: {}", name, checked.error().message()));
    }
    return checked;
}

bool allowedFile(Kind kind, std::string_view path)
{
    if (isNoticeFile(path))
    {
        return true;
    }
    const std::string extension = lowerExtension(path);
    constexpr std::array kRice = {"toml", "png", "jpg", "jpeg", "bmp", "mp4", "m4v", "webm",
                                  "cur",  "ani", "wav", "ico",  "inf", "md",  "txt"};
    constexpr std::array kPlugin = {"toml", "luau", "slint", "png", "jpg",
                                    "jpeg", "svg",  "json",  "md",  "txt"};
    const auto has = [&](const auto& list) {
        return std::ranges::any_of(list, [&](const char* e) { return extension == e; });
    };
    return kind == Kind::Rice ? has(kRice) : has(kPlugin);
}

std::uint64_t maxFileBytes(std::string_view path)
{
    const std::string extension = lowerExtension(path);
    if (isVideo(extension))
    {
        return limits::kVideoMaxBytes;
    }
    if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "bmp")
    {
        return limits::kImageMaxBytes;
    }
    if (extension == "cur" || extension == "ani")
    {
        return limits::kCursorMaxBytes;
    }
    if (extension == "wav")
    {
        return limits::kSoundMaxBytes;
    }
    if (extension == "ico")
    {
        return limits::kIconMaxBytes;
    }
    if (extension == "inf")
    {
        return limits::kInfMaxBytes;
    }
    if (extension == "toml")
    {
        return limits::kManifestMaxBytes;
    }
    if (extension == "luau" || extension == "slint")
    {
        return kScriptMaxBytes;
    }
    if (extension == "svg")
    {
        return kSvgMaxBytes;
    }
    if (extension == "json")
    {
        return kJsonMaxBytes;
    }
    return kTextMaxBytes;
}

Result<void> checkContent(std::string_view path, std::span<const std::byte> head)
{
    const std::string extension = lowerExtension(path);
    if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "bmp")
    {
        const bool png = startsWith(head, "\x89PNG\r\n\x1a\n");
        const bool jpeg = head.size() >= 3 && std::to_integer<int>(head[0]) == 0xFF &&
                          std::to_integer<int>(head[1]) == 0xD8 && std::to_integer<int>(head[2]) == 0xFF;
        const bool bmp = startsWith(head, "BM") && head.size() >= 26;
        if ((extension == "png" && !png) || ((extension == "jpg" || extension == "jpeg") && !jpeg) ||
            (extension == "bmp" && !bmp))
        {
            return bad(path, "the file is not what its name says");
        }
        std::optional<domain::Size> size;
        if (bmp)
        {
            const auto width = static_cast<std::int32_t>(le32(head, 18));
            const auto height = static_cast<std::int32_t>(le32(head, 22));
            if (width > 0 && height != 0 && height != INT32_MIN)
            {
                size = domain::Size{static_cast<std::uint32_t>(width),
                                    static_cast<std::uint32_t>(height < 0 ? -height : height)};
            }
        }
        else
        {
            size = domain::rasterImageSize(head);
        }
        if (!size || size->width == 0 || size->height == 0)
        {
            return bad(path, "the image's size cannot be read");
        }
        if (size->width > limits::kImageMaxSide || size->height > limits::kImageMaxSide ||
            static_cast<std::uint64_t>(size->width) * size->height > limits::kImageMaxPixels)
        {
            return bad(path, std::format("the image is too large ({}x{})", size->width, size->height));
        }
        return {};
    }
    if (extension == "cur" || extension == "ani")
    {
        if (auto checked = domain::validateCursorFile(head, extension); !checked)
        {
            return bad(path, checked.error().message());
        }
        return {};
    }
    if (extension == "ico")
    {
        if (auto checked = domain::validateIconFile(head); !checked)
        {
            return bad(path, checked.error().message());
        }
        return {};
    }
    if (extension == "wav")
    {
        if (auto checked = domain::validateWaveFile(head); !checked)
        {
            return bad(path, checked.error().message());
        }
        return {};
    }
    if (isVideo(extension))
    {
        if (extension == "webm")
        {
            // A WebM is an EBML document: it starts with the EBML header's id.
            if (!startsWith(head, "\x1A\x45\xDF\xA3"))
            {
                return bad(path, "not a WebM video");
            }
            return {};
        }
        // An MP4 starts with a box: 4 bytes of size, then "ftyp".
        if (head.size() < 8 || !startsWith(head.subspan(4), "ftyp"))
        {
            return bad(path, "not an MP4 video");
        }
        return {};
    }
    if (extension == "svg")
    {
        const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
        if (text.find('\0') != std::string_view::npos || text.find('<') == std::string_view::npos)
        {
            return bad(path, "not an SVG image");
        }
        return {};
    }
    if (isText(extension) || isNoticeFile(path))
    {
        const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
        if (text.find('\0') != std::string_view::npos || !isValidUtf8(text))
        {
            return bad(path, "not UTF-8 text");
        }
        return {};
    }
    return {}; // install.inf: read by the cursor module's own parser when applied
}

Result<Imported> extract(const fs::path& archive, Kind kind, const fs::path& staging)
{
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_win32w_create(archive.c_str(), 0, -1, &error);
    if (source == nullptr)
    {
        const std::string why = zip_error_strerror(&error);
        zip_error_fini(&error);
        return fail(ErrorCode::Io, std::format("cannot open the package: {}", why));
    }
    zip_error_fini(&error);
    return extractFrom(source, kind, staging);
}

Result<Imported> extract(std::span<const std::byte> archive, Kind kind, const fs::path& staging)
{
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_buffer_create(archive.data(), archive.size(), 0, &error);
    zip_error_fini(&error);
    if (source == nullptr)
    {
        return fail(ErrorCode::Io, "cannot read the package");
    }
    return extractFrom(source, kind, staging);
}

Result<std::uint64_t> pack(const fs::path& folder, Kind kind, const fs::path& out)
{
    auto root = platform::finalPath(folder);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    if (kind == Kind::Rice)
    {
        // A rice with others' files in it (a Wallpaper Engine wallpaper) is for this computer only.
        if (auto bytes = platform::readFileLimited(*root / L"rice.toml", limits::kManifestMaxBytes))
        {
            domain::Diagnostics diagnostics;
            const auto rice = domain::parseRice(
                std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), diagnostics);
            if (rice && !rice->meta.canShare())
            {
                return fail(ErrorCode::AccessDenied,
                            "this rice holds wallpapers that belong to their authors (from Wallpaper Engine): it "
                            "is yours to use on this computer, not to share");
            }
        }
    }
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(*root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const std::string name = utf16To8Lossy(it->path().filename().native());
        const auto status = it->symlink_status(ec);
        // Editor files (.git, .vscode, .luaurc), type definitions, Explorer's own files and
        // packages stay out.
        const std::string lower = lowerName(name);
        if (name.starts_with('.') || lower.ends_with(".d.luau") || lower.ends_with(".rmwp") ||
            lower.ends_with(".rice") || lower == "thumbs.db" || lower == "desktop.ini")
        {
            if (fs::is_directory(status))
            {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (fs::is_symlink(status) || fs::is_other(status))
        {
            return fail(ErrorCode::AccessDenied, std::format("{}: links are not allowed in a package", name));
        }
        if (fs::is_directory(status))
        {
            continue;
        }
        const std::string relative = utf16To8Lossy(fs::relative(it->path(), *root, ec).generic_wstring());
        auto valid = checkEntryName(relative);
        if (!valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        // What an install would refuse is refused here, where its author can fix it.
        if (!allowedFile(kind, *valid))
        {
            return fail(ErrorCode::Unsupported, std::format("{}: a {} cannot carry this kind of file", *valid,
                                                            kind == Kind::Rice ? "rice" : "plugin"));
        }
        files.emplace_back(std::move(*valid), it->path());
    }
    if (ec)
    {
        return fail(ErrorCode::Io, ec.message());
    }
    if (files.size() > limits::kPackageMaxEntries)
    {
        return fail(ErrorCode::LimitExceeded, std::format("more than {} files", limits::kPackageMaxEntries));
    }
    std::ranges::sort(files);

    // Written beside the target, which it replaces only when complete.
    const fs::path partial = fs::path(out.native() + L".partial");
    auto cleanup = ScopeExit([&]() noexcept {
        std::error_code ignored;
        fs::remove(partial, ignored);
    });
    std::ofstream zip(partial, std::ios::binary | std::ios::trunc);
    if (!zip)
    {
        return fail(ErrorCode::Io, "cannot write the package");
    }
    constexpr std::uint32_t kUtf8Names = 0x0800;
    constexpr std::uint32_t kDosDate = 0x0021; // 1980-01-01: the same files make the same package
    std::vector<std::byte> directory;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    std::vector<char> buffer(kChunk);
    for (const auto& [name, path] : files)
    {
        // Twice through the file: its CRC first (a stored entry carries it before the data).
        std::ifstream in(path, std::ios::binary);
        std::uint32_t crc = 0;
        std::uint64_t size = 0;
        std::vector<std::byte> content; // what checkContent() looks at
        const std::size_t keep = isVideo(lowerExtension(name)) ? kVideoHead : maxFileBytes(name);
        while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
        {
            const auto chunk = std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(in.gcount())));
            crc = crc32(crc, chunk);
            size += chunk.size();
            if (size > maxFileBytes(name))
            {
                return fail(ErrorCode::LimitExceeded,
                            std::format("{}: larger than {} MB", name, maxFileBytes(name) >> 20));
            }
            const auto take = std::min(chunk.size(), keep - std::min(keep, content.size()));
            content.insert(content.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
        }
        total += size;
        if (total > limits::kPackageMaxTotalBytes)
        {
            return fail(ErrorCode::LimitExceeded, "the package would be larger than 1 GB");
        }
        if (auto checked = checkContent(name, content); !checked)
        {
            return std::unexpected(std::move(checked.error()));
        }
        std::vector<std::byte> local;
        put32(local, 0x04034b50);
        put16(local, 10);
        put16(local, kUtf8Names);
        put16(local, 0); // stored
        put16(local, 0);
        put16(local, kDosDate);
        put32(local, crc);
        put32(local, static_cast<std::uint32_t>(size));
        put32(local, static_cast<std::uint32_t>(size));
        put16(local, static_cast<std::uint32_t>(name.size()));
        put16(local, 0);
        putText(local, name);
        zip.write(reinterpret_cast<const char*>(local.data()), static_cast<std::streamsize>(local.size()));
        in.clear();
        in.seekg(0);
        std::uint64_t copied = 0;
        while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
        {
            zip.write(buffer.data(), in.gcount());
            copied += static_cast<std::uint64_t>(in.gcount());
        }
        if (copied != size)
        {
            return fail(ErrorCode::Busy, std::format("{}: changed while it was packed", name));
        }

        put32(directory, 0x02014b50);
        put16(directory, 20);
        put16(directory, 10);
        put16(directory, kUtf8Names);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, kDosDate);
        put32(directory, crc);
        put32(directory, static_cast<std::uint32_t>(size));
        put32(directory, static_cast<std::uint32_t>(size));
        put16(directory, static_cast<std::uint32_t>(name.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, static_cast<std::uint32_t>(offset));
        putText(directory, name);
        offset += local.size() + size;
    }
    std::vector<std::byte> end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<std::uint32_t>(files.size()));
    put16(end, static_cast<std::uint32_t>(files.size()));
    put32(end, static_cast<std::uint32_t>(directory.size()));
    put32(end, static_cast<std::uint32_t>(offset));
    put16(end, 0);
    zip.write(reinterpret_cast<const char*>(directory.data()),
              static_cast<std::streamsize>(directory.size()));
    zip.write(reinterpret_cast<const char*>(end.data()), static_cast<std::streamsize>(end.size()));
    zip.close();
    if (!zip)
    {
        return fail(ErrorCode::Io, "cannot write the package");
    }
    if (auto replaced = platform::replaceFile(partial, out); !replaced)
    {
        return std::unexpected(std::move(replaced.error()));
    }
    cleanup.release();
    return offset + directory.size() + end.size();
}

} // namespace rmw::package
