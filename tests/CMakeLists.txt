add_executable(dragon-tests
    TestEnvironment.cpp
    core/SignalTest.cpp
    core/EventLoopTest.cpp
    core/FileWatcherTest.cpp
    config/ConfigTest.cpp
    wayland/ProtocolTest.cpp
    render/VulkanClearTest.cpp
    backend/HeadlessBackendTest.cpp
    server/SurfaceTest.cpp
    server/XdgShellTest.cpp
    desktop/DesktopRenderTest.cpp
    desktop/DmabufTest.cpp
    desktop/InputTest.cpp
    xwayland/XwaylandTest.cpp
    ${PROJECT_SOURCE_DIR}/compositor/core/LsanSuppressions.cpp
)
target_link_libraries(dragon-tests PRIVATE
    dragon_wayland
    dragon_xwayland
    dragon_client_protocols
    dragon::options
    GTest::gtest_main
)

pkg_check_modules(XcbTest REQUIRED IMPORTED_TARGET xcb)
target_link_libraries(dragon-tests PRIVATE PkgConfig::XcbTest)

include(GoogleTest)
gtest_discover_tests(dragon-tests)
