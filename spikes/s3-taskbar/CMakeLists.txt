find_package(wil CONFIG REQUIRED)

add_executable(spike_s3_taskbar main.cpp)
target_link_libraries(spike_s3_taskbar PRIVATE rmw::base rmw::platform WIL::WIL ole32 oleaut32 shell32 user32 advapi32 dwmapi)
rmw_configure_target(spike_s3_taskbar)
