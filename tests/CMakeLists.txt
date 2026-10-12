add_executable(dragon-tests
    core/SignalTest.cpp
    core/EventLoopTest.cpp
)
target_link_libraries(dragon-tests PRIVATE dragon_core dragon::options GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(dragon-tests)
