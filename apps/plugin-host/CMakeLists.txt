# rmw-plugin-host.exe — the Luau plugins of the community (docs/12 §3, P2), in a worker the core
# supervises. The only process that runs plugin code.

add_executable(rmw_plugin_host
    src/main.cpp
    src/HostApp.cpp
    src/Http.cpp
    src/Links.cpp
    src/ProviderHub.cpp
)
set_target_properties(rmw_plugin_host PROPERTIES OUTPUT_NAME "rmw-plugin-host")
target_link_libraries(rmw_plugin_host PRIVATE
    rmw::bar
    rmw::base
    rmw::ipc
    rmw::json
    rmw::platform
    rmw::plugins
    rmw::providers
    rmw::scripting
    user32
    winhttp
    ws2_32
)
rmw_configure_target(rmw_plugin_host)
