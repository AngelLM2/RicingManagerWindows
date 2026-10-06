# rmw::machine — the machine-wide icons of the advanced mode (M14, ADR-0021, doc 10 §4.1) without
# Windows: rmw-elevated's closed command line and the journal it undoes from. rmw-elevated
# (apps/elevated) does the Windows part, as administrator.

rmw_add_library(machine
    SOURCES
        src/MachineIcons.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
)
