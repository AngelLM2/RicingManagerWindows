// Spike S9 (M14): may a desktop app (not packaged) set the lock screen image, what does Windows give
// back, and which per-user registry values move with it? Prints what it finds. Run it only in
// Windows Sandbox (.\scripts\sandbox.ps1 -Suite spike): it changes the lock screen.

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
using winrt::Windows::Storage::Streams::IRandomAccessStream;
using winrt::Windows::System::UserProfile::LockScreen;

std::vector<std::uint8_t> readAll(const IRandomAccessStream& stream)
{
    const auto size = static_cast<std::uint32_t>(stream.Size());
    const DataReader reader(stream.GetInputStreamAt(0));
    reader.LoadAsync(size).get();
    std::vector<std::uint8_t> bytes(size);
    reader.ReadBytes(bytes);
    return bytes;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void printValues(HKEY root, const wchar_t* path)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        std::printf("  [%ls] (absent)\n", path);
        return;
    }
    std::printf("  [%ls]\n", path);
    for (DWORD i = 0;; ++i)
    {
        std::array<wchar_t, 256> name{};
        auto nameSize = static_cast<DWORD>(name.size());
        std::array<std::uint8_t, 1024> data{};
        auto dataSize = static_cast<DWORD>(data.size());
        DWORD type = 0;
        if (RegEnumValueW(key, i, name.data(), &nameSize, nullptr, &type, data.data(), &dataSize) != ERROR_SUCCESS)
        {
            break;
        }
        const std::wstring_view valueName(name.data(), nameSize);
        if (std::wstring_view(path).find(L"ContentDeliveryManager") != std::wstring_view::npos &&
            valueName.find(L"Lock") == std::wstring_view::npos && valueName.find(L"Rotating") == std::wstring_view::npos)
        {
            continue;
        }
        if (type == REG_DWORD)
        {
            DWORD value = 0;
            std::memcpy(&value, data.data(), sizeof(value));
            std::printf("    %ls = %lu\n", name.data(), value);
        }
        else if (type == REG_SZ || type == REG_EXPAND_SZ)
        {
            std::printf("    %ls = \"%ls\"\n", name.data(), reinterpret_cast<const wchar_t*>(data.data()));
        }
        else
        {
            std::printf("    %ls (type %lu, %lu bytes)\n", name.data(), type, dataSize);
        }
    }
    RegCloseKey(key);
}

void printRegistry(const char* when)
{
    std::printf("registry %s:\n", when);
    printValues(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)");
    printValues(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Lock Screen)");
    printValues(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Lock Screen\Creative)");
    printValues(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\PersonalizationCSP)");
}

std::vector<std::uint8_t> current(const char* when)
{
    try
    {
        const auto uri = LockScreen::OriginalImageFile();
        std::printf("%s: original image file: %ls\n", when, uri ? uri.AbsoluteUri().c_str() : L"(none)");
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("%s: original image file: error 0x%08X %ls\n", when, static_cast<unsigned>(e.code().value),
                    e.message().c_str());
    }
    try
    {
        auto bytes = readAll(LockScreen::GetImageStream());
        std::printf("%s: image stream: %zu bytes, starts %02X %02X %02X %02X\n", when, bytes.size(),
                    bytes.size() > 0 ? bytes[0] : 0, bytes.size() > 1 ? bytes[1] : 0, bytes.size() > 2 ? bytes[2] : 0,
                    bytes.size() > 3 ? bytes[3] : 0);
        return bytes;
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("%s: image stream: error 0x%08X %ls\n", when, static_cast<unsigned>(e.code().value),
                    e.message().c_str());
    }
    return {};
}

bool setFile(const std::filesystem::path& path)
{
    try
    {
        const auto file = StorageFile::GetFileFromPathAsync(path.native()).get();
        LockScreen::SetImageFileAsync(file).get();
        std::printf("set %ls: ok\n", path.c_str());
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        std::printf("set %ls: error 0x%08X %ls\n", path.c_str(), static_cast<unsigned>(e.code().value),
                    e.message().c_str());
        return false;
    }
}

} // namespace

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    printRegistry("before");
    const auto before = current("before");

    // A picture of Windows' own, copied to a folder of ours (as the store would hold it).
    const std::filesystem::path work = std::filesystem::current_path();
    std::filesystem::path source;
    for (const auto* candidate : {LR"(C:\Windows\Web\Screen\img100.jpg)", LR"(C:\Windows\Web\Screen\img101.png)",
                                  LR"(C:\Windows\Web\Wallpaper\Windows\img0.jpg)"})
    {
        if (std::filesystem::exists(candidate))
        {
            source = candidate;
            break;
        }
    }
    if (source.empty())
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(LR"(C:\Windows\Web)"))
        {
            if (entry.path().extension() == L".jpg")
            {
                source = entry.path();
                break;
            }
        }
    }
    std::printf("source: %ls\n", source.c_str());
    const auto copy = work / (L"lock" + source.extension().native());
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing);
    const auto sent = readFile(copy);

    if (setFile(copy))
    {
        Sleep(1000);
        const auto after = current("after set");
        std::printf("after set: same bytes as the file: %s (file %zu bytes)\n", after == sent ? "yes" : "no",
                    sent.size());
        printRegistry("after set");

        // The same again: does Windows report it as the same?
        setFile(copy);
        const auto again = current("after the same again");
        std::printf("again: same bytes as before it: %s\n", again == after ? "yes" : "no");
    }

    // Back to what it was, from the bytes alone (what a restore would have).
    if (!before.empty())
    {
        try
        {
            const InMemoryRandomAccessStream memory;
            const DataWriter writer(memory);
            writer.WriteBytes(before);
            writer.StoreAsync().get();
            writer.FlushAsync().get();
            writer.DetachStream();
            memory.Seek(0);
            LockScreen::SetImageStreamAsync(memory).get();
            const auto back = current("after the stream back");
            std::printf("back: same bytes as before: %s\n", back == before ? "yes" : "no");
        }
        catch (const winrt::hresult_error& e)
        {
            std::printf("stream back: error 0x%08X %ls\n", static_cast<unsigned>(e.code().value),
                        e.message().c_str());
        }
    }

    // Spotlight on through the per-user value, as Settings leaves it: does the image change?
    HKEY cdm = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)", 0,
                        nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &cdm, nullptr) == ERROR_SUCCESS)
    {
        const DWORD one = 1;
        RegSetValueExW(cdm, L"RotatingLockScreenEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one),
                       sizeof(one));
        RegCloseKey(cdm);
        Sleep(5000);
        current("after RotatingLockScreenEnabled=1");
        printRegistry("after RotatingLockScreenEnabled=1");
    }
    std::printf("done\n");
    return 0;
}
