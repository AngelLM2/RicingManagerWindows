# rmw.exe — command-line client of rmw-core (docs/04 M1).

add_executable(rmw_cli src/main.cpp src/PluginTools.cpp)
set_target_properties(rmw_cli PROPERTIES OUTPUT_NAME "rmw")
# rmw::scripting only compiles scripts (rmw plugin validate): the CLI never runs plugin code.
target_link_libraries(rmw_cli PRIVATE rmw::bar rmw::base rmw::domain rmw::ipc rmw::json rmw::platform rmw::plugins
                                      rmw::scripting)
rmw_configure_target(rmw_cli)
