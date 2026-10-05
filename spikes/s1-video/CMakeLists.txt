find_package(wil CONFIG REQUIRED)

add_executable(spike_s1 main.cpp)
target_link_libraries(spike_s1 PRIVATE rmw::base rmw::platform WIL::WIL
    d3d11 dxgi dcomp mfplat mfuuid mfreadwrite ole32 oleaut32 user32 gdi32)
rmw_configure_target(spike_s1)
