# rmw::base — portable utilities with no Windows headers (docs/03 §3).

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rmw/base/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/rmw/base/Version.hpp"
    @ONLY
)

rmw_add_library(base
    SOURCES
        src/Error.cpp
        src/FailFast.cpp
        src/Log.cpp
        src/RotatingFileSink.cpp
        src/Utf.cpp
)

target_include_directories(rmw_base PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
