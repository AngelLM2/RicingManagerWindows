# rmw-core.exe — the single writer: engine, state, IPC server, apply helper (docs/03 §2).

add_executable(rmw_core
    src/ApplyHelper.cpp
    src/Composition.cpp
    src/CoreServer.cpp
    src/Import.cpp
    src/Plugins.cpp
    src/Resident.cpp
    src/main.cpp
    ${PROJECT_SOURCE_DIR}/apps/resources/app.rc
)
set_target_properties(rmw_core PROPERTIES OUTPUT_NAME "rmw-core")
target_link_libraries(rmw_core PRIVATE
    rmw::bar
    rmw::base
    rmw::domain
    rmw::engine
    rmw::ipc
    rmw::json
    rmw::machine
    rmw::modules
    rmw::package
    rmw::platform
    rmw::plugins
    rmw::tiling
    rmw::video
)
rmw_configure_target(rmw_core)
