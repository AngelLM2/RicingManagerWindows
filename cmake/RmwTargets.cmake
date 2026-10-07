# Helpers that apply the project-wide build policy (docs/08 §3) to every first-party target.

include_guard(GLOBAL)

include(Warnings)
include(Hardening)
include(Sanitizers)

# The version resource every program carries (FileVersion, ProductVersion).
configure_file("${CMAKE_CURRENT_LIST_DIR}/version.rc.in" "${PROJECT_BINARY_DIR}/generated/version.rc" @ONLY)

# Applies warnings, hardening and sanitizers to a first-party target.
function(rmw_configure_target target)
    rmw_set_warnings(${target})
    rmw_harden(${target})
    rmw_sanitize(${target})

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${PROJECT_BINARY_DIR}/generated/version.rc")
    endif()
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

# Generated code we do not own (e.g. sources produced by slint_target_sources) is compiled
# without first-party warning policy; our own sources in the same target keep /W4 /WX.
function(rmw_relax_generated_sources target pattern)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        if(source MATCHES "${pattern}")
            set_source_files_properties("${source}" TARGET_DIRECTORY ${target}
                PROPERTIES COMPILE_OPTIONS "/W0;/WX-")
        endif()
    endforeach()
endfunction()
