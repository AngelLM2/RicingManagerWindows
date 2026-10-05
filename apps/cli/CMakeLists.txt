# rmw.exe — command-line client (docs/04 M1). For now it only proves the toolchain end to end.

add_executable(rmw_cli src/main.cpp)
set_target_properties(rmw_cli PROPERTIES OUTPUT_NAME "rmw")
target_link_libraries(rmw_cli PRIVATE rmw::base rmw::platform)
rmw_configure_target(rmw_cli)
