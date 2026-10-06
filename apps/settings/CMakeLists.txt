# rmw-settings.exe — the settings and gallery of Ricing Manager (docs/04 M7, docs/07, ADR-0007):
# Slint with its software renderer, a client of rmw-core, gone when its window closes.

# What it does with files and the rice model, without UI: always built (the unit tests use it).
add_library(rmw_settings_model STATIC model/src/Model.cpp model/src/WallpaperEngine.cpp)
add_library(rmw::settings_model ALIAS rmw_settings_model)
target_include_directories(rmw_settings_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/model/include")
target_link_libraries(rmw_settings_model PUBLIC rmw::base rmw::domain PRIVATE rmw::json rmw::platform)
rmw_configure_target(rmw_settings_model)

include(RmwSlint)
if(NOT RMW_HAVE_SLINT)
    message(STATUS "rmw-settings skipped: no Slint SDK")
    return()
endif()

find_package(wil CONFIG REQUIRED)

add_executable(rmw_settings WIN32
    src/main.cpp
    src/SettingsApp.cpp
    src/Shell.cpp
    src/Thumbnails.cpp
    ${PROJECT_SOURCE_DIR}/apps/resources/app.rc
)
set_target_properties(rmw_settings PROPERTIES
    OUTPUT_NAME "rmw-settings"
    # pt-BR (and any other language) from lang/<language>/LC_MESSAGES/rmw-settings.po, built in.
    SLINT_BUNDLE_TRANSLATIONS "${CMAKE_CURRENT_SOURCE_DIR}/lang"
    SLINT_TRANSLATION_DOMAIN "rmw-settings"
    SLINT_NO_DEFAULT_TRANSLATION_CONTEXT ON
)
slint_target_sources(rmw_settings ui/app.slint)
target_link_libraries(rmw_settings PRIVATE
    rmw::base
    rmw::client
    rmw::domain
    rmw::ipc
    rmw::json
    rmw::package
    rmw::platform
    rmw::plugins
    rmw::settings_model
    rmw::video
    Slint::Slint
    WIL::WIL
    dwmapi
    ole32
    oleaut32
    psapi
    shell32
    shlwapi
    user32
    windowscodecs
)
# The .po files are inputs of the generated code: a change rebuilds it.
file(GLOB_RECURSE RMW_SETTINGS_PO CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lang/*.po")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RMW_SETTINGS_PO})
rmw_use_slint_runtime(rmw_settings)
rmw_configure_target(rmw_settings)
rmw_relax_generated_sources(rmw_settings "slint_generated_")
