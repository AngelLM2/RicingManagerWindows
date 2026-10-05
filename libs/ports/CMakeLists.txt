# rmw::ports — the interfaces modules use to reach Windows (docs/03 §3). Pure C++, header-only:
# rmw::platform implements them with Win32/COM, tests implement them with fakes.

add_library(rmw_ports INTERFACE)
add_library(rmw::ports ALIAS rmw_ports)
target_include_directories(rmw_ports INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rmw_ports INTERFACE rmw::base)
