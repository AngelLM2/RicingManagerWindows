# rmw-core.exe — the single writer: engine, state, IPC server, apply helper (docs/03 §2).

add_executable(rmw_core
    src/ApplyHelper.cpp
    src/Composition.cpp
    src/CoreServer.cpp
    src/main.cpp
)
set_target_properties(rmw_core PROPERTIES OUTPUT_NAME "rmw-core")
target_link_libraries(rmw_core PRIVATE
    rmw::base
    rmw::domain
    rmw::engine
    rmw::ipc
    rmw::json
    rmw::modules
    rmw::platform
)
rmw_configure_target(rmw_core)
