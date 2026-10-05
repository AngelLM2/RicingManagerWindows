# rmw::platform — the only layer that talks to Win32/COM (docs/03 §3).

find_package(wil CONFIG REQUIRED)

rmw_add_library(platform
    SOURCES
        src/Apps.cpp
        src/Autostart.cpp
        src/Channel.cpp
        src/Console.cpp
        src/Customizers.cpp
        src/FileSystem.cpp
        src/MessageWindow.cpp
        src/NativeTaskbar.cpp
        src/Pipe.cpp
        src/Process.cpp
        src/Supervisor.cpp
        src/ProcessHardening.cpp
        src/Win32Display.cpp
        src/Win32Registry.cpp
        src/Win32Shell.cpp
        src/Win32ThemeApi.cpp
        src/WinError.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::ipc
        rmw::ports
    PRIVATE_DEPS
        WIL::WIL
        advapi32
        bcrypt
        ole32
        oleaut32
        shell32
        user32
        wtsapi32
)
