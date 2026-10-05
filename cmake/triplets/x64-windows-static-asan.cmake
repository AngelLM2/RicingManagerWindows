# Dependencies for the asan preset: instrumented with AddressSanitizer too, so container
# annotations (string/vector/optional) match first-party code and overflows inside
# third-party code are caught as well.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_C_FLAGS "/fsanitize=address")
set(VCPKG_CXX_FLAGS "/fsanitize=address")
