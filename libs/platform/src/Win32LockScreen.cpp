#include "rmw/platform/Win32Ports.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <shlwapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

namespace rmw::platform
{
namespace
{

namespace fs = std::filesystem;

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::System::UserProfile::LockScreen;

using Clock = std::chrono::steady_clock;

// Each call, whole, within what the apply helper gives this module (45 s), even on a busy PC.
constexpr auto kBudget = std::chrono::seconds(30);
constexpr auto kSettle = std::chrono::milliseconds(100);
constexpr auto kSettleAtMost = std::chrono::seconds(3); // Windows takes the new picture in about a second (spike S9)

// WinRT's blocking calls belong on an MTA thread: each call gets its own, like the theme calls.
template <typename T>
Result<T> onMta(const std::function<Result<T>()>& work)
{
    std::optional<Result<T>> result;
    std::thread thread([&] {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            result.emplace(work());
        }
        catch (const winrt::hresult_error& e)
        {
            result.emplace(fail(ErrorCode::System, std::format("lock screen: {}", winrt::to_string(e.message())),
                                e.code().value));
        }
        catch (const std::exception& e)
        {
            result.emplace(fail(ErrorCode::System, std::format("lock screen: {}", e.what())));
        }
        winrt::uninit_apartment();
    });
    thread.join();
    return std::move(*result);
}

// Waits until deadline; an operation still running then is cancelled. One that failed throws its
// error from GetResults() (the callers call it).
template <typename Operation>
Result<void> waitFor(const Operation& operation, std::string_view what, Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (operation.wait_for(std::max(left, std::chrono::milliseconds(1))) == AsyncStatus::Started)
    {
        operation.Cancel();
        return fail(ErrorCode::Timeout, std::format("lock screen: {} did not finish in time", what));
    }
    return {};
}

// "file:///C:/x.jpg" -> "C:\x.jpg"; "" for anything that is not a local file.
std::string sourceFile()
{
    const auto uri = LockScreen::OriginalImageFile();
    if (!uri || uri.SchemeName() != L"file")
    {
        return {};
    }
    std::array<wchar_t, 2048> path{};
    auto size = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(uri.AbsoluteUri().c_str(), path.data(), &size, 0)))
    {
        return {};
    }
    return utf16To8Lossy(path.data());
}

bool fileThere(const std::string& file)
{
    const auto wide = utf8To16(file);
    return wide && !wide->empty() && GetFileAttributesW(wide->c_str()) != INVALID_FILE_ATTRIBUTES;
}

// FNV-1a: names a saved picture by its content, so a later capture of another picture never
// overwrites one a snapshot points at.
std::uint64_t contentHash(const std::vector<std::uint8_t>& bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::uint8_t b : bytes)
    {
        hash = (hash ^ b) * 1099511628211ULL;
    }
    return hash;
}

// What Windows shows, saved under %TEMP%\RicingManagerWindows.
Result<std::string> savedPicture(Clock::time_point deadline)
{
    const auto stream = LockScreen::GetImageStream();
    if (!stream)
    {
        return fail(ErrorCode::NotFound, "lock screen: Windows gives no picture");
    }
    const auto size = stream.Size();
    if (size == 0 || size > limits::kImageMaxBytes)
    {
        return fail(ErrorCode::LimitExceeded, "lock screen: the picture is empty or too large");
    }
    const DataReader reader(stream.GetInputStreamAt(0));
    const auto loading = reader.LoadAsync(static_cast<std::uint32_t>(size));
    if (auto waited = waitFor(loading, "reading the picture", deadline); !waited)
    {
        return std::unexpected(std::move(waited.error()));
    }
    loading.GetResults();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    reader.ReadBytes(bytes);
    const bool png = bytes.size() > 4 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
    std::array<wchar_t, MAX_PATH + 1> temp{};
    if (GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) == 0)
    {
        return fail(ErrorCode::System, "lock screen: no temporary folder", GetLastError());
    }
    const fs::path folder = fs::path(temp.data()) / L"RicingManagerWindows";
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path file =
        folder / std::format(L"lockscreen-{:016x}.{}", contentHash(bytes), png ? L"png" : L"jpg");
    if (!fs::exists(file, ec))
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            return fail(ErrorCode::Io, "lock screen: cannot save the picture");
        }
    }
    return utf16To8Lossy(file.native());
}

} // namespace

Result<std::string> Win32LockScreen::currentImage()
{
    return onMta<std::string>([]() -> Result<std::string> {
        if (std::string source = sourceFile(); !source.empty() && fileThere(source))
        {
            return source;
        }
        return savedPicture(Clock::now() + kBudget);
    });
}

Result<std::string> Win32LockScreen::imageSource()
{
    return onMta<std::string>([]() -> Result<std::string> { return sourceFile(); });
}

Result<void> Win32LockScreen::setImage(const std::string& file)
{
    return onMta<void>([&file]() -> Result<void> {
        const auto deadline = Clock::now() + kBudget;
        auto wide = utf8To16(file);
        if (!wide)
        {
            return std::unexpected(std::move(wide.error()));
        }
        const auto opening = StorageFile::GetFileFromPathAsync(*wide);
        if (auto waited = waitFor(opening, "opening the picture", deadline); !waited)
        {
            return waited;
        }
        const auto setting = LockScreen::SetImageFileAsync(opening.GetResults());
        if (auto waited = waitFor(setting, "setting the picture", deadline); !waited)
        {
            return waited;
        }
        setting.GetResults();
        // Windows takes it in the background: wait until it names the file (what capture reads).
        const auto settled = std::min(deadline, Clock::now() + kSettleAtMost);
        while (Clock::now() < settled)
        {
            if (const std::string source = sourceFile(); CompareStringOrdinal(utf8To16Lossy(source).c_str(), -1,
                                                                             wide->c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                return {};
            }
            std::this_thread::sleep_for(kSettle);
        }
        log::warn("lockscreen: Windows does not name {} yet", file);
        return {};
    });
}

bool Win32LockScreen::exists(const std::string& file)
{
    return fileThere(file);
}

} // namespace rmw::platform
