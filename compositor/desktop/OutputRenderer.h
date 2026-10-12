#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "render/vulkan/VulkanRenderer.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace dragon {

class Desktop;
class Gpu;
class Output;
class Server;
class Surface;
class SurfaceTextures;

// Draws the desktop on one output each time it asks for a frame, then tells
// the drawn clients (wl_surface.enter, frame callbacks).
class OutputRenderer {
public:
    OutputRenderer(Server& server, Gpu& gpu, Desktop& desktop, SurfaceTextures& textures, Output& output);
    ~OutputRenderer();
    OutputRenderer(const OutputRenderer&) = delete;
    OutputRenderer& operator=(const OutputRenderer&) = delete;

    static constexpr std::array<float, 4> kBackground{0.08f, 0.09f, 0.12f, 1.0f};

private:
    void render();
    // Adds `surface` and its subsurfaces, in stacking order, with `origin` as its top-left.
    void collect(Surface* surface, Point origin);
    void enter(Surface* surface);

    Server& m_server;
    Gpu& m_gpu;
    Desktop& m_desktop;
    SurfaceTextures& m_textures;
    Output& m_output;
    Connection m_frame;

    std::vector<vk::TextureDraw> m_draws;
    std::vector<Surface*> m_drawn;
    std::unordered_map<Surface*, Connection> m_entered; // surfaces that got wl_surface.enter
};

} // namespace dragon
