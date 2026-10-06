#pragma once

#include "rmw/ports/Display.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rmw::test
{

class FakeDisplayTopology final : public ports::IDisplayTopology
{
public:
    Result<std::vector<ports::MonitorInfo>> monitors() override { return list; }

    // Two side by side: primary 2560x1440 at the origin, a 1920x1080 one to its left.
    static std::vector<ports::MonitorInfo> twoMonitors()
    {
        return {
            {R"(\\?\DISPLAY#AAA#1)", "AAA-1", "Main", true, 0, 0, 2560, 1440},
            {R"(\\?\DISPLAY#BBB#2)", "BBB-2", "Side", false, -1920, 0, 0, 1080},
        };
    }

    std::vector<ports::MonitorInfo> list = twoMonitors();
};

// IDesktopWallpaper in memory; files are real paths so sniffing can look at them.
class FakeWallpaperApi final : public ports::IWallpaperApi
{
public:
    Result<void> setDesktopWallpaper(std::string_view file) override
    {
        desktopWallpaper = std::string(file);
        for (auto& [path, shown] : files)
        {
            shown = std::string(file);
        }
        return {};
    }
    std::string desktopWallpaper;

    Result<std::vector<ports::WallpaperMonitor>> monitors() override
    {
        std::vector<ports::WallpaperMonitor> named;
        for (const auto& monitor : FakeDisplayTopology::twoMonitors())
        {
            named.push_back({monitor.devicePath, monitor.left, monitor.top, monitor.right, monitor.bottom});
        }
        return named;
    }
    Result<std::string> wallpaper(std::string_view devicePath) override
    {
        const auto it = files.find(std::string(devicePath));
        return it == files.end() ? std::string() : it->second;
    }
    Result<void> setWallpaper(std::string_view devicePath, std::string_view file) override
    {
        files[std::string(devicePath)] = std::string(file);
        slideshowState = {};
        ++setCalls;
        return {};
    }
    Result<ports::WallpaperPosition> position() override { return currentPosition; }
    Result<void> setPosition(ports::WallpaperPosition position) override
    {
        currentPosition = position;
        return {};
    }
    Result<std::uint32_t> backgroundColor() override { return background; }
    Result<void> setBackgroundColor(std::uint32_t rgb) override
    {
        background = rgb;
        return {};
    }
    Result<ports::SlideshowState> slideshow() override { return slideshowState; }
    Result<void> setSlideshow(const ports::SlideshowState& state) override
    {
        slideshowState = state;
        return {};
    }
    Result<std::string> transcodedWallpaper() override { return transcoded; }
    Result<std::string> sniffImageType(std::string_view file) override
    {
        std::ifstream in(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
        char magic[2]{};
        if (!in.read(magic, 2))
        {
            return std::string();
        }
        if (magic[0] == 'B' && magic[1] == 'M')
        {
            return std::string("bmp");
        }
        return std::string("jpg");
    }
    bool isWindowsFile(std::string_view file) override { return file.starts_with("C:\\Windows\\"); }

    std::map<std::string, std::string> files; // device path -> image
    ports::WallpaperPosition currentPosition = ports::WallpaperPosition::Fill;
    std::uint32_t background = 0x000000;
    ports::SlideshowState slideshowState;
    std::string transcoded;
    int setCalls = 0;
};

} // namespace rmw::test
