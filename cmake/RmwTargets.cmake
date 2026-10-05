# Helpers that apply the project-wide build policy (docs/08 §3) to every first-party target.

include_guard(GLOBAL)

include(Warnings)
include(Hardening)
include(Sanitizers)

# Applies warnings, hardening and sanitizers to a first-party target.
function(rmw_configure_target target)
    rmw_set_warnings(${target})
    rmw_harden(${target})
    rmw_sanitize(${target})

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" AND CMAKE_BUILD_TYPE STREQUAL "Release")
        # Keep PDBs for crash dumps, drop unreferenced code and fold identical functions.
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
endfunction()

# Declares a first-party static library under libs/<name> with the include layout
# libs/<name>/include/rmw/<name>/*.hpp and alias rmw::<name>.
function(rmw_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    add_library(rmw_${name} STATIC ${ARG_SOURCES})
    add_library(rmw::${name} ALIAS rmw_${name})
    target_include_directories(rmw_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(rmw_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    rmw_configure_target(rmw_${name})
endfunction()

function(rmw_enable_clang_tidy)
    find_program(
        RMW_CLANG_TIDY_EXE clang-tidy
        HINTS "$ENV{VCINSTALLDIR}Tools/Llvm/x64/bin"
        REQUIRED
    )
    # MSVC-only flags are unknown to clang; do not fail on them.
    set(CMAKE_CXX_CLANG_TIDY "${RMW_CLANG_TIDY_EXE};--extra-arg=-Wno-unknown-argument;--quiet"
        PARENT_SCOPE)
endfunction()
