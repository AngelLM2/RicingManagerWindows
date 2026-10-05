# rmw::platform — the only layer that talks to Win32/COM (docs/03 §3).

find_package(wil CONFIG REQUIRED)

rmw_add_library(platform
    SOURCES
        src/Console.cpp
        src/FileSystem.cpp
        src/ProcessHardening.cpp
        src/WinError.cpp
    PUBLIC_DEPS
        rmw::base
    PRIVATE_DEPS
        WIL::WIL
        bcrypt
)
