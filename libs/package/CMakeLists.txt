# rmw::package — reading and writing .rice and .rmwp packages (docs/06 §4). Only the import worker
# (rmw-core --import-worker) reads packages from outside; the CLI packs them.

find_package(libzip CONFIG REQUIRED)

rmw_add_library(package
    SOURCES
        src/Package.cpp
    PUBLIC_DEPS
        rmw::base
    PRIVATE_DEPS
        rmw::domain
        rmw::platform
        libzip::zip
)
