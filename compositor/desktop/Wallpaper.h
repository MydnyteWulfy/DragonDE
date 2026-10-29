#pragma once

#include "config/Settings.h"
#include "core/Geometry.h"
#include "render/vulkan/VulkanRenderer.h"

#include <memory>
#include <string>
#include <vector>

typedef struct _GdkPixbuf GdkPixbuf;

namespace dragon {

namespace vk {
class VulkanTexture;
}

// The desktop background image (any format gdk-pixbuf reads: PNG, JPEG,
// WebP, SVG, ...), scaled on the CPU to exactly what's drawn so the GPU only
// ever samples it 1:1.
class Wallpaper {
public:
    explicit Wallpaper(vk::VulkanRenderer& renderer);
    ~Wallpaper();
    Wallpaper(const Wallpaper&) = delete;
    Wallpaper& operator=(const Wallpaper&) = delete;

    // Loads the image (once per path). False if it can't be read.
    bool set(const WallpaperSettings& settings);
    // Appends what covers `screen` (nothing without an image).
    void collect(Rect screen, std::vector<vk::TextureDraw>& draws);

private:
    vk::VulkanRenderer& m_renderer;
    WallpaperSettings m_settings;
    GdkPixbuf* m_image = nullptr;
    // The scaled texture and what it was made for.
    std::unique_ptr<vk::VulkanTexture> m_texture;
    Rect m_screen;
    std::string m_mode;
    Rect m_placed; // where the scaled image goes (single image modes)
};

} // namespace dragon
