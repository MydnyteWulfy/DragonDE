pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)

add_executable(dragon-tests
    core/SignalTest.cpp
    core/EventLoopTest.cpp
    wayland/ProtocolTest.cpp
)
target_link_libraries(dragon-tests PRIVATE
    dragon_wayland
    PkgConfig::WaylandClient
    dragon::options
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(dragon-tests)
