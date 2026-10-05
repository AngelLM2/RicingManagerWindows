# rmw::engine — RiceEngine, journal, content store, module catalog (docs/03 §5).

rmw_add_library(engine
    SOURCES
        src/ContentStore.cpp
        src/ModuleCatalog.cpp
        src/RiceEngine.cpp
        src/StateDb.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
        rmw::storage
    PRIVATE_DEPS
        rmw::platform
)
