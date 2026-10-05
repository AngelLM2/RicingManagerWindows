# rmw::ipc — message framing and JSON-RPC 2.0 between the core and its clients/workers
# (docs/03 §8). Pure C++: the pipe transport is in rmw::platform.

rmw_add_library(ipc
    SOURCES
        src/Frame.cpp
        src/JsonRpc.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::json
)
