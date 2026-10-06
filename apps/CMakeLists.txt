# The Slint SDK once for every app that shows a UI (rmw-bar, rmw-settings): its imported targets
# and RMW_HAVE_SLINT are then seen by all of them.
include(RmwSlint)

add_subdirectory(cli)
add_subdirectory(core)
add_subdirectory(bar)
add_subdirectory(plugin-host)
add_subdirectory(wallpaper)
add_subdirectory(tiler)
add_subdirectory(settings)
