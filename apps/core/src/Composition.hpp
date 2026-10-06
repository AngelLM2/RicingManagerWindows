#pragma once

#include "rmw/engine/ModuleCatalog.hpp"
#include "rmw/modules/Appearance.hpp"
#include "rmw/modules/Cursors.hpp"
#include "rmw/modules/Icons.hpp"
#include "rmw/modules/LockScreen.hpp"
#include "rmw/modules/Sounds.hpp"
#include "rmw/modules/Taskbar.hpp"
#include "rmw/modules/Tweaks.hpp"
#include "rmw/modules/Wallpaper.hpp"
#include "rmw/platform/Win32Ports.hpp"

#include <memory>

// The real modules, wired to the Win32 ports. Both sets are pinned in memory (the modules hold
// references to the ports), so they are only handed out behind unique_ptr.
namespace rmw::core
{

// The halves that run in the core: no Explorer calls.
struct ModuleLogic
{
    platform::Win32DisplayTopology topology;
    modules::AppearanceLogic appearance;
    modules::TaskbarLogic taskbar;
    modules::CursorsLogic cursors;
    modules::SoundsLogic sounds;
    modules::IconsLogic icons;
    modules::LockScreenLogic lockscreen;
    modules::WallpaperLogic wallpaper{topology};
    modules::TweaksLogic tweaks;
    engine::ModuleCatalog catalog;

    ModuleLogic() = default;
    ModuleLogic(const ModuleLogic&) = delete;
    ModuleLogic& operator=(const ModuleLogic&) = delete;
    ModuleLogic(ModuleLogic&&) = delete;
    ModuleLogic& operator=(ModuleLogic&&) = delete;
    ~ModuleLogic() = default;
};

[[nodiscard]] Result<std::unique_ptr<ModuleLogic>> makeModuleLogic();

// The halves that talk to Windows (the --apply-helper, or the core with --in-process).
struct ModuleSystems
{
    platform::Win32Registry registry;
    platform::Win32Shell shell;
    platform::Win32Icons iconsApi;
    platform::Win32LockScreen lockScreenApi;
    platform::Win32ThemeApi theme;
    platform::Win32DisplayTopology topology;
    platform::Win32WallpaperApi wallpaperApi;
    modules::AppearanceSystem appearance{registry, shell, theme};
    modules::TaskbarSystem taskbar{registry, shell};
    modules::CursorsSystem cursors{registry, shell};
    modules::SoundsSystem sounds{registry, shell};
    modules::IconsSystem icons{registry, iconsApi};
    modules::LockScreenSystem lockscreen{registry, lockScreenApi};
    modules::WallpaperSystem wallpaper{topology, wallpaperApi, registry};
    modules::TweaksSystem tweaks{registry, shell};
    engine::LocalModuleRunner runner;

    ModuleSystems();
    ModuleSystems(const ModuleSystems&) = delete;
    ModuleSystems& operator=(const ModuleSystems&) = delete;
    ModuleSystems(ModuleSystems&&) = delete;
    ModuleSystems& operator=(ModuleSystems&&) = delete;
    ~ModuleSystems() = default;
};

} // namespace rmw::core
