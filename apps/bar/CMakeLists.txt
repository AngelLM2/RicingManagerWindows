# rmw-bar.exe — the custom taskbar (docs/04 M3): a worker the core supervises; it never runs
# plugin code (docs/12 §3). Needs the pinned Slint SDK (.\scripts\fetch-deps.ps1 -Name slint).

include(RmwSlint)
if(NOT RMW_HAVE_SLINT)
    message(STATUS "rmw-bar is not built: no Slint SDK (run .\\scripts\\fetch-deps.ps1 -Name slint)")
    return()
endif()

# Everything but main, so the bar's own tests link it too.
add_library(rmw_bar_ui STATIC
    src/BarApp.cpp
    src/BarWindow.cpp
    src/DockWidget.cpp
    src/MediaWidget.cpp
    src/MenuWindow.cpp
    src/PluginBridge.cpp
    src/PluginWidgets.cpp
    src/SlintPlatform.cpp
    src/StartMenu.cpp
    src/Surface.cpp
    src/SystemWidgets.cpp
    src/TrayForwarder.cpp
    src/TrayHost.cpp
    src/TrayWidget.cpp
    src/Widgets.cpp
    src/WindowList.cpp
    src/WinKeyHook.cpp
    src/WorkspacesWidget.cpp
)
target_include_directories(rmw_bar_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rmw_bar_ui PUBLIC
    rmw::bar
    rmw::base
    rmw::domain
    rmw::platform
    rmw::plugins
    rmw::providers
    rmw::tiling
    Slint::Slint
    comctl32
    dwmapi
    gdi32
    iphlpapi
    runtimeobject
    ole32
    powrprof
    secur32
    shcore
    shell32
    advapi32
    user32
)
rmw_configure_target(rmw_bar_ui)

add_executable(rmw_bar_app src/main.cpp)
set_target_properties(rmw_bar_app PROPERTIES OUTPUT_NAME "rmw-bar")
target_link_libraries(rmw_bar_app PRIVATE rmw_bar_ui)
rmw_use_slint_runtime(rmw_bar_app)
rmw_configure_target(rmw_bar_app)
