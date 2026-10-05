# rmw::modules — what each feature does to Windows, written against rmw::ports (docs/03 §3-4).

rmw_add_library(modules
    SOURCES
        src/Appearance.cpp
        src/Cursors.cpp
        src/Export.cpp
        src/RegistryFields.cpp
        src/Taskbar.cpp
        src/Wallpaper.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::engine
        rmw::json
        rmw::ports
)
