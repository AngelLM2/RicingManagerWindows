# rmw::providers — what the system is doing (CPU, memory, network, battery, volume, media, windows,
# virtual desktops, monitors), read the same way for the bar's own widgets and for plugins
# (docs/12 §1, dogfooding). Windows only; no UI toolkit.

find_package(wil CONFIG REQUIRED)

rmw_add_library(providers
    SOURCES
        src/Media.cpp
        src/System.cpp
        src/Volume.cpp
        src/Windows.cpp
    PUBLIC_DEPS
        rmw::base
    PRIVATE_DEPS
        WIL::WIL
        dwmapi
        iphlpapi
        ole32
        shell32
        gdi32
        runtimeobject
        shcore
        user32
)
