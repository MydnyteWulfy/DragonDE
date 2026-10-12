cmake_minimum_required(VERSION 3.28)
project(DragonDE VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(DRAGON_SANITIZE "Enable ASan + UBSan in Debug builds" ON)
option(DRAGON_BUILD_TESTS "Build unit and integration tests" ON)

include(cmake/DragonCompileOptions.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandServer REQUIRED IMPORTED_TARGET wayland-server>=1.23)
include(cmake/DragonProtocols.cmake)
include(cmake/DragonShaders.cmake)

add_subdirectory(tools/scanner)
add_subdirectory(compositor)
add_subdirectory(tools/demo-client)

if(DRAGON_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()
