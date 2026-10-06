cmake_minimum_required(VERSION 3.28)

project(
    RicingManagerWindows
    VERSION 1.3.0
    DESCRIPTION "Open-source, lightweight and reversible Windows customizer"
    HOMEPAGE_URL "https://github.com/AngelLM2/RicingManagerWindows"
    LANGUAGES CXX
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "RicingManagerWindows builds only on Windows with MSVC (see docs/08).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules here: skip the per-file dependency scan (faster builds, and clean
# compile_commands.json for scripts/tidy.ps1).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Every executable side by side (rmw.exe looks for rmw-core.exe next to itself).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Static CRT by choice (docs/06 §5). Must match the vcpkg triplet in cmake/triplets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Always produce PDBs, including Release (crash dumps need them).
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

option(RMW_HARDENING "Enable CFG, EH continuation and CET mitigations" ON)
option(RMW_SPECTRE "Compile with /Qspectre and link the Spectre-mitigated CRT" OFF)
option(RMW_REQUIRE_SPECTRE "Fail the configure step if the Spectre-mitigated CRT is missing" OFF)
option(RMW_ASAN "Build with AddressSanitizer" OFF)
option(RMW_BUILD_TESTS "Build the test suite" ON)
option(RMW_BUILD_SPIKES "Build the throwaway Fase 0 spikes (spikes/)" OFF)
option(RMW_FUZZ "Build only the libFuzzer targets (fuzz/), with coverage; needs RMW_ASAN" OFF)

if(RMW_FUZZ AND NOT RMW_ASAN)
    message(FATAL_ERROR "RMW_FUZZ needs RMW_ASAN (use the fuzz preset).")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(RmwTargets)

add_subdirectory(libs)
if(RMW_FUZZ)
    # The parsers and their fuzz targets; the apps and tests are not built with coverage.
    add_subdirectory(fuzz)
    return()
endif()
add_subdirectory(apps)
# The official rices: drawn and packed by the `rices` target (not part of `all`).
add_subdirectory(rices)

if(RMW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RMW_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()
