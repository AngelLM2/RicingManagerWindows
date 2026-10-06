# rmw-wallpaper.exe — live wallpapers behind the desktop icons (docs/04 M6, M12), in a worker the core
# supervises: Media Foundation (DXVA), the D3D11 video processor, Direct2D (effects, particles), WIC
# (pictures) and DirectComposition.

find_package(wil CONFIG REQUIRED)

add_executable(rmw_wallpaper
    src/main.cpp
    src/Composer.cpp
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
    d2d1
    d3d11
    dcomp
    dwmapi
    dxgi
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    shell32
    user32
    windowscodecs
    wtsapi32
)
rmw_configure_target(rmw_wallpaper)
