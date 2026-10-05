# rmw::domain — pure C++ model of a rice: parsing, validation, rules. Never includes <windows.h>
# (docs/03 §3).

find_package(tomlplusplus CONFIG REQUIRED)

rmw_add_library(domain
    SOURCES
        src/Color.cpp
        src/CursorFiles.cpp
        src/Diagnostics.cpp
        src/Identifiers.cpp
        src/Images.cpp
        src/MonitorSelector.cpp
        src/PathRules.cpp
        src/Rice.cpp
        src/RiceParser.cpp
        src/SemVer.cpp
        src/TomlReader.cpp
    PUBLIC_DEPS
        rmw::base
    PRIVATE_DEPS
        tomlplusplus::tomlplusplus
)
