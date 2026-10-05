# rmw::bar — what the custom taskbar draws and where (docs/04 M3), without Win32 or Slint: the
# plan the core hands rmw-bar, the geometry of a bar on a monitor, and the Slint source generated
# from the rice's zones. rmw-bar (apps/bar) puts it on screen.

rmw_add_library(bar
    SOURCES
        src/Bridge.cpp
        src/Geometry.cpp
        src/Plan.cpp
        src/Source.cpp
        src/WidgetSource.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
)
