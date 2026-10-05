# Static CRT + static libraries, built with the same exploit mitigations as first-party code
# (docs/06 §3). /sdl and /WX are intentionally NOT applied to third-party code.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_C_FLAGS "/guard:cf /guard:ehcont /Gy")
set(VCPKG_CXX_FLAGS "/guard:cf /guard:ehcont /Gy")
set(VCPKG_LINKER_FLAGS "/guard:cf /guard:ehcont /CETCOMPAT")
