# The official rices (docs/11). Each folder here holds a rice's sources (rice.toml, CREDITS.md);
# its pictures, video loop and cursors are drawn at build time by rmw-rice-art (original art from
# code, nothing downloaded) and the whole rice is packed by `rmw pack`, which checks it like an
# import would. Not part of `all` (the videos take a while):
#   cmake --build --preset release --target rices
# Out: <build>\rices\<Name>\ (the folder the installer puts in the library) and <build>\rices\<id>.rice.

find_package(wil CONFIG REQUIRED)

add_executable(rmw_rice_art art/RiceArt.cpp)
set_target_properties(rmw_rice_art PROPERTIES OUTPUT_NAME "rmw-rice-art")
target_link_libraries(rmw_rice_art PRIVATE WIL::WIL d2d1 d3d11 dxgi dxguid dwrite windowscodecs mfplat mfreadwrite mfuuid
                                           ole32)
rmw_configure_target(rmw_rice_art)

# id|folder name (the rice's name, as an import would call its folder)
set(RMW_RICES
    "diva-stage|Diva Stage"
    "tokyo-midnight|Tokyo Midnight"
    "sakura-dawn|Sakura Dawn"
    "mocha-kawaii|Mocha Kawaii"
    "your-rice|Your Rice")

set(packages)
foreach(entry IN LISTS RMW_RICES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 id)
    list(GET parts 1 name)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${id}")
    set(folder "${CMAKE_BINARY_DIR}/rices/${name}")
    set(package "${CMAKE_BINARY_DIR}/rices/${id}.rice")
    file(GLOB sources CONFIGURE_DEPENDS "${source}/*")
    add_custom_command(
        OUTPUT "${package}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${folder}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${source}" "${folder}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/LICENSE" "${folder}/LICENSE"
        COMMAND rmw_rice_art ${id} "${folder}"
        COMMAND rmw_cli pack "${folder}" --out "${package}"
        DEPENDS ${sources} rmw_rice_art rmw_cli "${PROJECT_SOURCE_DIR}/LICENSE"
        COMMENT "Drawing and packing the rice ${name}"
        VERBATIM)
    list(APPEND packages "${package}")
endforeach()
add_custom_target(rices DEPENDS ${packages})
