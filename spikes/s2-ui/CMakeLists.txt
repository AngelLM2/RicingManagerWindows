# Needs the pinned Slint SDK: .\scripts\fetch-deps.ps1 -Name slint
set(RMW_SLINT_DIR "${PROJECT_SOURCE_DIR}/build/.deps/slint-1.18.1")
if(NOT EXISTS "${RMW_SLINT_DIR}/lib/cmake/Slint/SlintConfig.cmake")
    message(STATUS "spike_s2: Slint SDK not found, run scripts/fetch-deps.ps1 -Name slint")
    return()
endif()

find_package(Slint CONFIG REQUIRED PATHS "${RMW_SLINT_DIR}/lib/cmake/Slint" NO_DEFAULT_PATH)
find_package(wil CONFIG REQUIRED)

add_executable(spike_s2 main.cpp)
slint_target_sources(spike_s2 gallery.slint)
target_link_libraries(spike_s2 PRIVATE rmw::base rmw::platform Slint::Slint WIL::WIL ole32 oleaut32)
# slint_cpp.dll ships next to the exe (docs/06 §5): allow the application directory.
set_target_properties(spike_s2 PROPERTIES RMW_APP_DIR_DLLS ON)
rmw_configure_target(spike_s2)
rmw_relax_generated_sources(spike_s2 "slint_generated_")

# slint_cpp.dll links the dynamic VC++ runtime: ship it app-local (no admin-only redistributable).
file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _rmw_redist)
file(GLOB RMW_VC_RUNTIME_DLLS "${_rmw_redist}/x64/Microsoft.VC*.CRT/msvcp140.dll"
                              "${_rmw_redist}/x64/Microsoft.VC*.CRT/vcruntime140.dll"
                              "${_rmw_redist}/x64/Microsoft.VC*.CRT/vcruntime140_1.dll")
if(NOT RMW_VC_RUNTIME_DLLS)
    message(WARNING "VC++ runtime DLLs not found under '${_rmw_redist}'; slint_cpp.dll needs them app-local.")
endif()
add_custom_command(TARGET spike_s2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RMW_SLINT_DIR}/lib/slint_cpp.dll" ${RMW_VC_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:spike_s2>"
)
