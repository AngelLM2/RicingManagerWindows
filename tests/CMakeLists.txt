find_package(GTest CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
include(GoogleTest)

add_executable(rmw_unit_tests
    unit/CheckedCastTests.cpp
    unit/ConsoleTests.cpp
    unit/DatabaseTests.cpp
    unit/DomainValueTests.cpp
    unit/ErrorTests.cpp
    unit/FileSystemTests.cpp
    unit/JsonTests.cpp
    unit/LogTests.cpp
    unit/MonitorSelectorTests.cpp
    unit/PathRulesTests.cpp
    unit/RiceEngineTests.cpp
    unit/RiceParserTests.cpp
    unit/ScopeExitTests.cpp
    unit/StatementTests.cpp
    unit/UtfTests.cpp
    unit/WinErrorTests.cpp
)
target_include_directories(rmw_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(rmw_unit_tests PRIVATE
    rmw::base
    rmw::domain
    rmw::engine
    rmw::json
    rmw::platform
    rmw::storage
    GTest::gtest
    GTest::gtest_main
    unofficial::sqlite3::sqlite3
)
rmw_configure_target(rmw_unit_tests)

# NO_PRETTY_VALUES: parameterized tests keep their generated names instead of raw parameter bytes.
gtest_discover_tests(rmw_unit_tests DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES)
