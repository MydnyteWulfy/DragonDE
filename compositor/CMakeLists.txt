add_library(dragon_core STATIC
    core/EventLoop.cpp
    core/Log.cpp
    core/Png.cpp
    core/Process.cpp
)
pkg_check_modules(Pixman REQUIRED IMPORTED_TARGET pixman-1)

target_include_directories(dragon_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dragon_core
    PUBLIC PkgConfig::WaylandServer PkgConfig::Pixman
    PRIVATE dragon::options
)

# wl_interface tables for every protocol, shared by server and client code.
add_library(dragon_protocol_tables STATIC)
target_include_directories(dragon_protocol_tables PUBLIC ${DRAGON_PROTOCOL_ROOT} ${DRAGON_PROTOCOL_DIR})
target_link_libraries(dragon_protocol_tables PUBLIC PkgConfig::WaylandServer)

# Wayland object model + every generated server-side protocol binding.
add_library(dragon_wayland STATIC
    wayland/Global.cpp
    wayland/Resource.cpp
)
target_link_libraries(dragon_wayland PUBLIC dragon_core dragon_protocol_tables PRIVATE dragon::options)

dragon_add_protocol(dragon_wayland ${WAYLAND_XML_DIR}/wayland.xml CORE SKIP wl_display wl_registry)
dragon_add_protocol(dragon_wayland ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
dragon_add_protocol(dragon_wayland ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml)

# GPU: render node, GBM allocation, dma-buf sync, swapchains, Vulkan renderer.
find_package(Vulkan REQUIRED)
pkg_check_modules(Gbm REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LibDrm REQUIRED IMPORTED_TARGET libdrm)

add_library(dragon_render STATIC
    render/DmaBuf.cpp
    render/GbmAllocator.cpp
    render/Gpu.cpp
    render/RenderNode.cpp
    render/Swapchain.cpp
    render/vulkan/Formats.cpp
    render/vulkan/VulkanDevice.cpp
    render/vulkan/VulkanImage.cpp
    render/vulkan/VulkanRenderer.cpp
    render/vulkan/VulkanTexture.cpp
)
target_link_libraries(dragon_render
    PUBLIC dragon_core Vulkan::Vulkan PkgConfig::Gbm PkgConfig::LibDrm
    PRIVATE dragon::options
)
dragon_add_shaders(dragon_render
    render/vulkan/shaders/quad.vert
    render/vulkan/shaders/texture.frag
)

# Client-side protocol headers: the nested backend, tools and tests are clients.
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)

add_library(dragon_client_protocols INTERFACE)
target_link_libraries(dragon_client_protocols INTERFACE dragon_protocol_tables PkgConfig::WaylandClient)
dragon_add_client_protocol(dragon_client_protocols ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
dragon_add_client_protocol(dragon_client_protocols
    ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml)
dragon_add_client_protocol(dragon_client_protocols
    ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
dragon_add_client_protocol(dragon_client_protocols
    ${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml)
# cursor-shape references zwp_tablet_tool_v2, so its tables must exist too.
dragon_add_client_protocol(dragon_client_protocols ${WAYLAND_PROTOCOLS_DIR}/stable/tablet/tablet-v2.xml)

# Backends: where outputs and input come from.
add_library(dragon_backend STATIC
    backend/Backend.cpp
    backend/headless/HeadlessBackend.cpp
    backend/nested/NestedBackend.cpp
)
target_link_libraries(dragon_backend
    PUBLIC dragon_render
    PRIVATE dragon_client_protocols dragon::options
)

# Server side: protocol implementations and the globals that expose them.
add_library(dragon_server STATIC
    Server.cpp
    protocols/Buffer.cpp
    protocols/Compositor.cpp
    protocols/DataDevice.cpp
    protocols/LinuxDmabuf.cpp
    protocols/Output.cpp
    protocols/Seat.cpp
    protocols/XdgShell.cpp
)
pkg_check_modules(XkbCommon REQUIRED IMPORTED_TARGET xkbcommon)
target_link_libraries(dragon_server
    PUBLIC dragon_wayland dragon_backend
    PRIVATE PkgConfig::XkbCommon dragon::options
)

# What's on screen: window placement, surface textures, per-output rendering.
add_library(dragon_desktop STATIC
    desktop/Desktop.cpp
    desktop/InputRouter.cpp
    desktop/OutputRenderer.cpp
    desktop/SurfaceTextures.cpp
)
target_link_libraries(dragon_desktop
    PUBLIC dragon_server dragon_render
    PRIVATE dragon::options
)

add_executable(dragon-compositor main.cpp)
target_link_libraries(dragon-compositor PRIVATE dragon_desktop dragon::options)
