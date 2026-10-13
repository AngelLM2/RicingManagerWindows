find_package(GTest CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-luau CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
include(GoogleTest)

add_executable(rmw_unit_tests
    unit/AppearanceTaskbarTests.cpp
    unit/BarTests.cpp
    unit/CheckedCastTests.cpp
    unit/ConsoleTests.cpp
    unit/CursorFilesTests.cpp
    unit/CursorsModuleTests.cpp
    unit/DatabaseTests.cpp
    unit/DomainValueTests.cpp
    unit/ErrorTests.cpp
    unit/ExportTests.cpp
    unit/FileSystemTests.cpp
    unit/IconPixelsTests.cpp
    unit/IconsTests.cpp
    unit/LockScreenTests.cpp
    unit/MachineIconsTests.cpp
    unit/MessageWindowTests.cpp
    unit/IpcTests.cpp
    unit/JsonTests.cpp
    unit/LifecycleTests.cpp
    unit/LogTests.cpp
    unit/MonitorSelectorTests.cpp
    unit/PackageTests.cpp
    unit/PathRulesTests.cpp
    unit/PluginManifestTests.cpp
    unit/PluginPlumbingTests.cpp
    unit/PluginRuntimeTests.cpp
    unit/PluginTypesTests.cpp
    unit/RiceEngineTests.cpp
    unit/RiceParserTests.cpp
    unit/RiceWriterTests.cpp
    unit/SettingsModelTests.cpp
    unit/ScopeExitTests.cpp
    unit/SettingsTests.cpp
    unit/SoundsTests.cpp
    unit/StatementTests.cpp
    unit/StateDbTests.cpp
    unit/SupervisorTests.cpp
    unit/TilingTests.cpp
    unit/TrayTests.cpp
    unit/TweaksModuleTests.cpp
    unit/UtfTests.cpp
    unit/VideoTests.cpp
    unit/WallpaperEngineTests.cpp
    unit/WallpaperModuleTests.cpp
    unit/WidgetSourceTests.cpp
    unit/Win32PortsTests.cpp
    unit/WinErrorTests.cpp
)
target_include_directories(rmw_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support"
                                                  # rmw's command table (CoreNeed.hpp, header only)
                                                  "${PROJECT_SOURCE_DIR}/apps/cli/src")
target_link_libraries(rmw_unit_tests PRIVATE
    rmw::bar
    rmw::base
    rmw::settings_model
    rmw::domain
    rmw::engine
    rmw::ipc
    rmw::json
    rmw::machine
    rmw::modules
    rmw::package
    rmw::platform
    rmw::plugins
    rmw::providers
    rmw::scripting
    rmw::storage
    rmw::tiling
    rmw::video
    GTest::gtest
    GTest::gtest_main
    unofficial::sqlite3::sqlite3
    # Zips made the way other tools make them (deflated), for the package tests.
    libzip::zip
    # Luau's analyzer: type-checks the shipped plugin scripts against rmw.d.luau (tests only).
    unofficial::luau::Luau.Analysis
)
# The shipped plugins and their type definitions are read from the source tree.
target_compile_definitions(rmw_unit_tests PRIVATE RMW_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
rmw_configure_target(rmw_unit_tests)

# A worker that leaks, hangs, crashes or exits on purpose, for the supervisor tests.
add_executable(rmw_test_worker support/TestWorker.cpp)
target_link_libraries(rmw_test_worker PRIVATE rmw::platform)
rmw_configure_target(rmw_test_worker)
add_dependencies(rmw_unit_tests rmw_test_worker)

# A tray icon the M5 Sandbox tests click (tests/system/sandbox/tray-tests.ps1).
add_executable(rmw_tray_test_app WIN32 support/TrayTestApp.cpp)
target_link_libraries(rmw_tray_test_app PRIVATE shell32 user32)
target_link_options(rmw_tray_test_app PRIVATE /ENTRY:wmainCRTStartup) # no console window: only a tray icon
rmw_configure_target(rmw_tray_test_app)
add_dependencies(rmw_unit_tests rmw_tray_test_app)

# A test video and windows that cover the desktop, for the M6 Sandbox tests (video-tests.ps1).
find_package(wil CONFIG REQUIRED)
add_executable(rmw_video_test_tool support/VideoTestTool.cpp)
target_link_libraries(rmw_video_test_tool PRIVATE WIL::WIL mf mfplat mfreadwrite mfuuid ole32 user32 gdi32)
rmw_configure_target(rmw_video_test_tool)
add_dependencies(rmw_unit_tests rmw_video_test_tool)

# Plain app windows the M13 Sandbox tests tile (tiler-tests.ps1).
add_executable(rmw_tiler_test_app WIN32 support/TilerTestApp.cpp)
target_link_libraries(rmw_tiler_test_app PRIVATE advapi32 gdi32 shell32 user32)
rmw_configure_target(rmw_tiler_test_app)
add_dependencies(rmw_unit_tests rmw_tiler_test_app)

# NO_PRETTY_VALUES: parameterized tests keep their generated names instead of raw parameter bytes.
gtest_discover_tests(rmw_unit_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)

# The bar's drawing, without windows (needs the Slint SDK, like rmw-bar).
if(TARGET rmw_bar_ui)
    add_executable(rmw_bar_tests bar/BarUiTests.cpp bar/StartTests.cpp)
    target_link_libraries(rmw_bar_tests PRIVATE rmw_bar_ui GTest::gtest GTest::gtest_main)
    # The example plugins are compiled into a bar by the tests.
    target_compile_definitions(rmw_bar_tests PRIVATE RMW_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    rmw_use_slint_runtime(rmw_bar_tests)
    rmw_configure_target(rmw_bar_tests)
    gtest_discover_tests(rmw_bar_tests DISCOVERY_MODE PRE_TEST)
endif()
