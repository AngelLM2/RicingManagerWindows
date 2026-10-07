# Slint SDK (pinned, fetched by scripts/fetch-deps.ps1 -Name slint) — docs/adr/0007.
include_guard(GLOBAL)

set(RMW_SLINT_DIR "${PROJECT_SOURCE_DIR}/build/.deps/slint-1.18.1")
# Directory variables stay in the scope that included this file first; the function reads this.
set_property(GLOBAL PROPERTY RMW_SLINT_DIR "${RMW_SLINT_DIR}")
set(RMW_HAVE_SLINT FALSE)
if(EXISTS "${RMW_SLINT_DIR}/lib/cmake/Slint/SlintConfig.cmake")
    find_package(Slint CONFIG REQUIRED PATHS "${RMW_SLINT_DIR}/lib/cmake/Slint" NO_DEFAULT_PATH)
    set(RMW_HAVE_SLINT TRUE)
elseif(RMW_REQUIRE_SLINT)
    # Without it the bar and the settings window are left out, and an older copy of them still in bin\
    # went into the v1.8.0 installer: the settings window of v1.7 with the core of v1.8.
    message(FATAL_ERROR "Slint SDK not found in ${RMW_SLINT_DIR}; run: .\\scripts\\fetch-deps.ps1 -Name slint "
                        "(or configure with -DRMW_REQUIRE_SLINT=OFF to build without the bar and the settings window)")
else()
    message(STATUS "Slint SDK not found; run: .\\scripts\\fetch-deps.ps1 -Name slint")
endif()

# Ships slint_cpp.dll and the app-local VC++ runtime it needs next to the executable, and lets
# the executable load DLLs from its own folder (docs/06 §5).
function(rmw_use_slint_runtime target)
    set_target_properties(${target} PROPERTIES RMW_APP_DIR_DLLS ON)
    get_property(slint_dir GLOBAL PROPERTY RMW_SLINT_DIR)
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" redist)
    file(GLOB runtime "${redist}/x64/Microsoft.VC*.CRT/msvcp140.dll"
                      "${redist}/x64/Microsoft.VC*.CRT/vcruntime140.dll"
                      "${redist}/x64/Microsoft.VC*.CRT/vcruntime140_1.dll")
    if(NOT runtime)
        message(WARNING "VC++ runtime DLLs not found under '${redist}'; slint_cpp.dll needs them app-local.")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${slint_dir}/lib/slint_cpp.dll" ${runtime}
                "$<TARGET_FILE_DIR:${target}>"
    )
endfunction()
