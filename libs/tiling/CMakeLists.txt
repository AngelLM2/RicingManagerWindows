# rmw::tiling — the tiler's decisions without Windows (docs/14 §M13): where windows go in each layout,
# the workspaces of each monitor, which shortcut fires, which rule applies, the plan the core hands
# rmw-tiler, the file that puts every window back and what the bars are told. rmw-tiler (apps/tiler)
# does it on screen.

rmw_add_library(tiling
    SOURCES
        src/Desktop.cpp
        src/Layout.cpp
        src/Plan.cpp
        src/Workspace.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
)
