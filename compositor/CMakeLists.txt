add_library(dragon_core STATIC
    core/EventLoop.cpp
    core/Log.cpp
)
target_include_directories(dragon_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dragon_core
    PUBLIC PkgConfig::WaylandServer
    PRIVATE dragon::options
)

add_executable(dragon-compositor main.cpp)
target_link_libraries(dragon-compositor PRIVATE dragon_core dragon::options)
