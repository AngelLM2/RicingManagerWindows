# rmw-elevated.exe — Ricing Manager's administrator part (doc 10 §4.1, ADR-0021): started through the
# UAC for one operation, a closed list of commands, nothing from the caller ever a registry key or a
# destination path. Today: the folder and drive icons of the whole PC (HKLM Shell Icons).

add_executable(rmw_elevated
    src/main.cpp
    ${PROJECT_SOURCE_DIR}/apps/resources/app.rc
)
set_target_properties(rmw_elevated PROPERTIES OUTPUT_NAME "rmw-elevated")
target_link_libraries(rmw_elevated PRIVATE
    rmw::base
    rmw::domain
    rmw::machine
    rmw::platform
    advapi32
    shell32
)
rmw_configure_target(rmw_elevated)
