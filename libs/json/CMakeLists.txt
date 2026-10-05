# rmw::json — JSON for snapshots, module configurations and IPC, on nlohmann-json, with input
# limits and no exceptions crossing the API (docs/06 §4).

find_package(nlohmann_json CONFIG REQUIRED)

rmw_add_library(json
    SOURCES
        src/Json.cpp
        src/Value.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        nlohmann_json::nlohmann_json
)
