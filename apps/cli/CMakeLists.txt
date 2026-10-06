# rmw.exe — command-line client of rmw-core (docs/04 M1).

# `rmw plugin new widget` writes the files of plugins/template (one source for both), with the API's
# type definitions: embedded at build time, each as one MSVC string literal (under 16 KB).
set(RMW_EMBEDDED "// Generated from plugins/template: do not edit.\n")
function(rmw_embed_text variable file)
    file(READ "${file}" content)
    string(LENGTH "${content}" length)
    if(length GREATER 16000 OR content MATCHES "\\)rmwtext\"")
        message(FATAL_ERROR "${file} must stay under 16000 bytes to be embedded in rmw.exe")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    set(RMW_EMBEDDED "${RMW_EMBEDDED}constexpr std::string_view ${variable} = R\"rmwtext(${content})rmwtext\";\n"
        PARENT_SCOPE)
endfunction()
set(RMW_TEMPLATE "${PROJECT_SOURCE_DIR}/plugins/template")
rmw_embed_text(kTemplateToml "${RMW_TEMPLATE}/plugin.toml")
rmw_embed_text(kTemplateLuau "${RMW_TEMPLATE}/main.luau")
rmw_embed_text(kTemplateSlint "${RMW_TEMPLATE}/widget.slint")
rmw_embed_text(kRmwTypes "${RMW_TEMPLATE}/rmw.d.luau")
rmw_embed_text(kLuauRc "${RMW_TEMPLATE}/.luaurc")
rmw_embed_text(kVsCodeSettings "${RMW_TEMPLATE}/.vscode/settings.json")
rmw_embed_text(kVsCodeExtensions "${RMW_TEMPLATE}/.vscode/extensions.json")
# Rewritten only when it changes, so a configure does not rebuild the CLI.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/PluginTemplate.inc.new" "${RMW_EMBEDDED}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/PluginTemplate.inc.new"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/PluginTemplate.inc" COPYONLY)

add_executable(rmw_cli src/main.cpp src/PluginTools.cpp)
set_target_properties(rmw_cli PROPERTIES OUTPUT_NAME "rmw")
target_include_directories(rmw_cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# rmw::scripting only compiles scripts (rmw plugin validate): the CLI never runs plugin code.
target_link_libraries(rmw_cli PRIVATE rmw::bar rmw::base rmw::client rmw::domain rmw::ipc rmw::json rmw::package
                                      rmw::platform rmw::plugins
                                      rmw::scripting)
rmw_configure_target(rmw_cli)
