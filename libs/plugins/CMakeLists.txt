# rmw::plugins — the Luau runtime of rmw-plugin-host (docs/12 §3, P2): sandboxed VMs, the rmw.*
# API and its permission checks, without Windows (the host process supplies data and I/O).

find_package(unofficial-luau CONFIG REQUIRED)

rmw_add_library(plugins
    SOURCES
        src/Runtime.cpp
        src/Url.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
    PRIVATE_DEPS
        unofficial::luau::Luau.VM
        unofficial::luau::Luau.Compiler
        unofficial::luau::Luau.Ast
        unofficial::luau::Luau.Common
)
