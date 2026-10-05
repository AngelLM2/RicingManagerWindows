find_package(wil CONFIG REQUIRED)

add_executable(spike_s8 main.cpp)
target_link_libraries(spike_s8 PRIVATE rmw::base rmw::platform WIL::WIL shell32 user32)
rmw_configure_target(spike_s8)
