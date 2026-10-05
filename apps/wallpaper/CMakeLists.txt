# rmw-wallpaper.exe — video wallpapers behind the desktop icons (docs/04 M6), in a worker the core
# supervises: Media Foundation (DXVA), the D3D11 video processor and DirectComposition.

find_package(wil CONFIG REQUIRED)

add_executable(rmw_wallpaper
    src/main.cpp
    src/Desktop.cpp
    src/Renderer.cpp
    src/WallpaperApp.cpp
)
set_target_properties(rmw_wallpaper PROPERTIES OUTPUT_NAME "rmw-wallpaper")
target_link_libraries(rmw_wallpaper PRIVATE
    rmw::base
    rmw::domain
    rmw::platform
    rmw::video
    WIL::WIL
    d3d11
    dcomp
    dwmapi
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    shell32
    user32
    wtsapi32
)
rmw_configure_target(rmw_wallpaper)
