# rmw-tiler.exe — tiling, workspaces and their shortcuts (docs/14 §M13): a worker the core supervises,
# off until the person turns it on. It moves other apps' windows from the outside (WinEvents out of
# context, SetWindowPos, DWM's border color); nothing is injected.

add_executable(rmw_tiler
    src/Animator.cpp
    src/KeyHook.cpp
    src/Looks.cpp
    src/SharedBlock.cpp
    src/TilerApp.cpp
    src/VirtualDesktops.cpp
    src/WindowOps.cpp
    src/main.cpp
    ${PROJECT_SOURCE_DIR}/apps/resources/app.rc
)
set_target_properties(rmw_tiler PROPERTIES OUTPUT_NAME "rmw-tiler")
target_link_libraries(rmw_tiler PRIVATE
    rmw::base
    rmw::domain
    rmw::platform
    rmw::tiling
    advapi32
    dwmapi
    ole32
    shcore
    user32
)
rmw_configure_target(rmw_tiler)