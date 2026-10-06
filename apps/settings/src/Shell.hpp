#pragma once

#include "rmw/base/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What rmw-settings asks of Windows besides the core: pickers, Explorer, its own window's frame,
// the processes of Ricing Manager, the logs.
namespace rmw::settingsapp
{

struct FileFilter
{
    std::wstring name;    // "Images"
    std::wstring pattern; // "*.jpg;*.jpeg;*.png;*.bmp"
};

// The Windows open dialog: a file (with filters) or a folder. Empty when cancelled.
[[nodiscard]] std::optional<std::filesystem::path> pickFile(void* owner,
                                                            const std::vector<FileFilter>& filters);
[[nodiscard]] std::optional<std::filesystem::path> pickFolder(void* owner);
// The Windows save dialog, for one kind of file (the extension is added when missing), starting
// at `name`. Asks before replacing a file. Empty when cancelled.
[[nodiscard]] std::optional<std::filesystem::path> pickSaveFile(void* owner, const FileFilter& filter,
                                                                const std::wstring& name);

// The first frame of a picture (a GIF preview) as a PNG file: the rice's wallpapers are PNG, JPEG
// or BMP.
[[nodiscard]] Result<void> savePng(const std::filesystem::path& picture, const std::filesystem::path& png);

// A folder in Explorer, when the user asks for it.
void openFolder(const std::filesystem::path& folder);

// The title bar follows the light/dark mode the window shows.
void setDarkTitleBar(void* window, bool dark);
[[nodiscard]] bool windowsUsesDarkApps();

// Windows' display language is Portuguese.
[[nodiscard]] bool windowsSpeaksPortuguese();

struct ProcessSample
{
    std::string name; // "rmw-core.exe"
    std::uint32_t pid = 0;
    std::uint64_t privateBytes = 0;
    double cpuPercent = 0.0; // of the whole machine, since the previous sample
};

// The processes of Ricing Manager (rmw*.exe). CPU is measured against the previous call.
class ProcessWatch
{
public:
    [[nodiscard]] std::vector<ProcessSample> sample();

private:
    struct Last
    {
        std::uint64_t cpu100ns = 0;
        std::uint64_t at100ns = 0;
    };
    std::map<std::uint32_t, Last> m_last;
};

// The newest warnings and errors of every log, newest first.
[[nodiscard]] std::vector<std::string> recentProblems(const std::filesystem::path& dataDirectory,
                                                      std::size_t max);

// <data>\support\<time>: the logs and `state` (JSON text), the user's folder hidden.
[[nodiscard]] Result<std::filesystem::path> makeSupportBundle(const std::filesystem::path& dataDirectory,
                                                              std::string_view state);

} // namespace rmw::settingsapp
