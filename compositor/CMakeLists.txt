add_library(dragon_core STATIC
    core/EventLoop.cpp
    core/Log.cpp
)
target_include_directories(dragon_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dragon_core
    PUBLIC PkgConfig::WaylandServer
    PRIVATE dragon::options
)

# Wayland object model + every generated protocol binding.
add_library(dragon_wayland STATIC
    wayland/Global.cpp
    wayland/Resource.cpp
)
target_include_directories(dragon_wayland PUBLIC ${DRAGON_PROTOCOL_ROOT} ${DRAGON_PROTOCOL_DIR})
target_link_libraries(dragon_wayland PUBLIC dragon_core PRIVATE dragon::options)

dragon_add_protocol(dragon_wayland ${WAYLAND_XML_DIR}/wayland.xml CORE SKIP wl_display wl_registry)
dragon_add_protocol(dragon_wayland ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
dragon_add_protocol(dragon_wayland ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml)

add_executable(dragon-compositor main.cpp)
target_link_libraries(dragon-compositor PRIVATE dragon_wayland dragon::options)
