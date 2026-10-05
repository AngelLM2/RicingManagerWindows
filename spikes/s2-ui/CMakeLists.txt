if(NOT RMW_HAVE_SLINT)
    return()
endif()

find_package(wil CONFIG REQUIRED)

add_executable(spike_s2 main.cpp)
slint_target_sources(spike_s2 gallery.slint)
target_link_libraries(spike_s2 PRIVATE rmw::base rmw::platform Slint::Slint WIL::WIL ole32 oleaut32)
rmw_use_slint_runtime(spike_s2)
rmw_configure_target(spike_s2)
rmw_relax_generated_sources(spike_s2 "slint_generated_")
