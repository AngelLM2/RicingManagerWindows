# rmw::client — a client of rmw-core over its pipe (docs/03 §8): the CLI and the settings app.

rmw_add_library(client
    SOURCES
        src/CoreClient.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::ipc
        rmw::json
    PRIVATE_DEPS
        rmw::platform
)
