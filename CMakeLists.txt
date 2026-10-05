cmake_minimum_required(VERSION 3.28)

project(
    RicingManagerWindows
    VERSION 0.1.0
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

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(RmwTargets)

add_subdirectory(libs)
add_subdirectory(apps)

if(RMW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RMW_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()
