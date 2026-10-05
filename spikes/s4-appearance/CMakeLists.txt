find_package(wil CONFIG REQUIRED)

add_executable(spike_s4 main.cpp)
target_link_libraries(spike_s4 PRIVATE rmw::base rmw::platform WIL::WIL ole32 oleaut32 shell32 user32 advapi32)
rmw_configure_target(spike_s4)
