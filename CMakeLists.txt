cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(
    RicingManagerWindows
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(
    GLOB_RECURSE RICING_MANAGER_SOURCES
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)

add_executable(
    RicingManagerWindows
    ${RICING_MANAGER_SOURCES}
)

target_include_directories(
    RicingManagerWindows
    PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)

target_compile_definitions(
    RicingManagerWindows
    PRIVATE
    UNICODE
    _UNICODE
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

target_link_libraries(
    RicingManagerWindows
    PRIVATE
    unofficial::sqlite3::sqlite3
    ole32
    user32
)