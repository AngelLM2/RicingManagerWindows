# rmw::plugins — what the core, the bar, the CLI and the plugin host share about plugins (docs/12):
# the plan the host runs, network addresses, reading a plugin's files with the same checks. No
# Luau: only rmw::scripting runs plugin code.

rmw_add_library(plugins
    SOURCES
        src/HostPlan.cpp
        src/PluginFiles.cpp
        src/Url.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
        rmw::platform
)
