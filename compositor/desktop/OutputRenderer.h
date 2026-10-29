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
class TitlebarRenderer;
class Wallpaper;
struct BlurStyle;
struct WallpaperSettings;

// Draws the desktop on one output each time it asks for a frame, then tells
// the drawn clients (wl_surface.enter, frame callbacks).
class OutputRenderer {
public:
    OutputRenderer(Server& server, Gpu& gpu, Desktop& desktop, SurfaceTextures& textures, Output& output);
    ~OutputRenderer();
    OutputRenderer(const OutputRenderer&) = delete;
    OutputRenderer& operator=(const OutputRenderer&) = delete;

    static constexpr std::array<float, 4> kBackground{0.08f, 0.09f, 0.12f, 1.0f};
    // What's behind the windows (sRGB).
    void setBackground(std::array<float, 4> rgba) { m_background = rgba; }
    void setWallpaper(const WallpaperSettings& settings);

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
    // How a window's surfaces are drawn: faded, clipped to its frame.
    struct SurfaceStyle {
        float alpha = 1.0f;
        std::optional<Rect> scissor;
        std::optional<vk::RoundedRect> clip;
    };
    // Adds `surface` and its subsurfaces, in stacking order, with `origin` as its top-left.
    void collect(Surface* surface, Point origin, const SurfaceStyle& style);
    void collect(Surface* surface, Point origin);
    // A window: shadow, glow, blur, border, title bar, its surfaces, dimming.
    void collectWindow(const Desktop::RenderItem& item);
    // A layer surface, styled by its layer rules.
    void collectLayer(const Desktop::RenderItem& item);
    // Frosted glass behind `region`.
    void addBackdrop(const vk::RoundedRect& region, const BlurStyle& blur, float alpha,
                     vk::VulkanTexture* mask = nullptr, Rect maskRect = {}, float maskThreshold = 0);
    void enter(Surface* surface);

    Server& m_server;
    Gpu& m_gpu;
    Desktop& m_desktop;
    SurfaceTextures& m_textures;
    Output& m_output;
    Connection m_frame;
    std::array<float, 4> m_background = kBackground;
    std::unique_ptr<TitlebarRenderer> m_titlebars;
    std::unique_ptr<Wallpaper> m_wallpaper;
    // Where windows start in m_draws: xray blur captures what's below there.
    std::optional<std::size_t> m_captureAt;
    bool m_captured = false;

    std::vector<vk::TextureDraw> m_draws;
    std::vector<Surface*> m_drawn;
    std::unordered_map<Surface*, Connection> m_entered; // surfaces that got wl_surface.enter
    std::string m_screenshotPath;
    std::vector<OverlayProvider> m_overlays;
};

} // namespace dragon
