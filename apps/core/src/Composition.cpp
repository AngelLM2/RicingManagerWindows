#include "Composition.hpp"

namespace rmw::core
{

Result<std::unique_ptr<ModuleLogic>> makeModuleLogic()
{
    auto logic = std::make_unique<ModuleLogic>();
    for (const engine::IModuleLogic* module : {static_cast<const engine::IModuleLogic*>(&logic->appearance),
                                               static_cast<const engine::IModuleLogic*>(&logic->taskbar),
                                               static_cast<const engine::IModuleLogic*>(&logic->cursors),
                                               static_cast<const engine::IModuleLogic*>(&logic->sounds),
                                               static_cast<const engine::IModuleLogic*>(&logic->icons),
                                               static_cast<const engine::IModuleLogic*>(&logic->lockscreen),
                                               static_cast<const engine::IModuleLogic*>(&logic->wallpaper),
                                               static_cast<const engine::IModuleLogic*>(&logic->tweaks)})
    {
        if (auto added = logic->catalog.add(*module); !added)
        {
            return std::unexpected(std::move(added.error()));
        }
    }
    if (auto finalized = logic->catalog.finalize(); !finalized)
    {
        return std::unexpected(std::move(finalized.error()));
    }
    return logic;
}

ModuleSystems::ModuleSystems()
{
    runner.add(appearance);
    runner.add(taskbar);
    runner.add(cursors);
    runner.add(sounds);
    runner.add(icons);
    runner.add(lockscreen);
    runner.add(wallpaper);
    runner.add(tweaks);
}

} // namespace rmw::core
