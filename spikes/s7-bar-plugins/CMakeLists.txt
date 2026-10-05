if(NOT RMW_HAVE_SLINT)
    return()
endif()

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-luau CONFIG REQUIRED)

add_executable(spike_s7 main.cpp)
target_link_libraries(spike_s7 PRIVATE rmw::base rmw::platform Slint::Slint WIL::WIL nlohmann_json::nlohmann_json
    unofficial::luau::Luau.VM unofficial::luau::Luau.Compiler unofficial::luau::Luau.Ast
    unofficial::luau::Luau.Common rpcrt4)
rmw_use_slint_runtime(spike_s7)
rmw_configure_target(spike_s7)
