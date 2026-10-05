# rmw::storage — SQLite databases (state.db, library.db) and config files (docs/03 §7).

find_package(unofficial-sqlite3 CONFIG REQUIRED)

rmw_add_library(storage
    SOURCES
        src/Database.cpp
    PUBLIC_DEPS
        rmw::base
    PRIVATE_DEPS
        unofficial::sqlite3::sqlite3
)
