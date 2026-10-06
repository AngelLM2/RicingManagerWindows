# libFuzzer targets for our own parsers (docs/06 §4, docs/08): everything that reads text or bytes
# from outside — rices, plugins, settings, cursor packs, packages, IPC, the plugin host, the tray,
# the workers' plans. Built only by the fuzz preset (AddressSanitizer + coverage); run them with
# scripts/fuzz.ps1, which seeds each one from the repository's own files.

function(rmw_add_fuzzer name)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    add_executable(fuzz_${name} ${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE ${ARG_DEPS})
    target_compile_options(fuzz_${name} PRIVATE /fsanitize=fuzzer)
    rmw_configure_target(fuzz_${name})
endfunction()

rmw_add_fuzzer(rice_toml DEPS rmw::domain)
rmw_add_fuzzer(plugin_toml DEPS rmw::domain)
rmw_add_fuzzer(settings_toml DEPS rmw::domain)
rmw_add_fuzzer(cursor_file DEPS rmw::domain)
rmw_add_fuzzer(cursor_inf DEPS rmw::domain)
rmw_add_fuzzer(tweaks DEPS rmw::domain rmw::modules)
rmw_add_fuzzer(ipc DEPS rmw::ipc rmw::json)
rmw_add_fuzzer(widget_slint DEPS rmw::bar)
rmw_add_fuzzer(bridge DEPS rmw::bar)
rmw_add_fuzzer(tray DEPS rmw::bar)
rmw_add_fuzzer(plans DEPS rmw::bar rmw::plugins rmw::video)
rmw_add_fuzzer(url DEPS rmw::plugins)
rmw_add_fuzzer(package DEPS rmw::package)
