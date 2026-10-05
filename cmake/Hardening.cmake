include_guard(GLOBAL)

# Locate the Spectre-mitigated CRT once. CMake + Ninja does not switch libraries automatically
# like MSBuild does, so the directory is added to the linker search path (searched before LIB).
if(RMW_SPECTRE)
    file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _rmw_vctools)
    set(RMW_SPECTRE_LIB_DIR "${_rmw_vctools}/lib/spectre/x64")
    if(NOT EXISTS "${RMW_SPECTRE_LIB_DIR}")
        if(RMW_REQUIRE_SPECTRE)
            message(FATAL_ERROR
                "Spectre-mitigated libraries not found at '${RMW_SPECTRE_LIB_DIR}'. "
                "Install 'MSVC Spectre-mitigated libs' in the Visual Studio Installer.")
        endif()
        message(WARNING "Spectre-mitigated libraries not found; building without them.")
        set(RMW_SPECTRE_LIB_DIR "")
    endif()
endif()

# Exploit mitigations (docs/06 §3 and §5). Not used with AddressSanitizer.
function(rmw_harden target)
    # Turns undefined behaviour in the STL (out-of-range operator[], etc.) into a fast fail.
    target_compile_definitions(${target} PRIVATE _MSVC_STL_HARDENING=1)

    if(NOT RMW_HARDENING)
        return()
    endif()

    target_compile_options(${target} PRIVATE /sdl /guard:cf /guard:ehcont /Gy)

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
        target_link_options(${target} PRIVATE /guard:cf /guard:ehcont /CETCOMPAT)
    endif()
    if(type STREQUAL "EXECUTABLE")
        # Static imports resolve only from System32 (docs/06 §5). Executables that ship a DLL
        # next to them (rmw-settings with slint_cpp.dll) set RMW_APP_DIR_DLLS to also allow
        # the application directory.
        get_target_property(app_dir_dlls ${target} RMW_APP_DIR_DLLS)
        if(app_dir_dlls)
            target_link_options(${target} PRIVATE /DEPENDENTLOADFLAG:0xA00)
        else()
            target_link_options(${target} PRIVATE /DEPENDENTLOADFLAG:0x800)
        endif()
    endif()

    if(RMW_SPECTRE)
        target_compile_options(${target} PRIVATE /Qspectre)
        if(RMW_SPECTRE_LIB_DIR AND (type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY"))
            target_link_options(${target} PRIVATE "/LIBPATH:${RMW_SPECTRE_LIB_DIR}")
        endif()
    endif()
endfunction()
