#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "desktop/Desktop.h"
#include "render/vulkan/VulkanRenderer.h"

#include <functional>
#include <optional>

#include <array>
#include <string>
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

    // Something drawn above everything else each frame: the client's cursor
    // image, a drag icon. Global coordinates; nullopt: nothing this frame.
    using OverlayProvider = std::function<std::optional<Desktop::RenderItem>()>;
    void addOverlay(OverlayProvider provider) { m_overlays.push_back(std::move(provider)); }

    // Saves the next frame as a PNG (debugging aid; renders it a second time
    // into a CPU-readable buffer).
    void screenshot(std::string path) { m_screenshotPath = std::move(path); }

private:
    void saveScreenshot();

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
    std::string m_screenshotPath;
    std::vector<OverlayProvider> m_overlays;
};

} // namespace dragon
