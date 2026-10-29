#include "desktop/Wallpaper.h"

#include "core/Log.h"
#include "render/vulkan/VulkanTexture.h"

#include <algorithm>
#include <cmath>
#include <gdk-pixbuf/gdk-pixbuf.h>

namespace dragon {

Wallpaper::Wallpaper(vk::VulkanRenderer& renderer) : m_renderer(renderer) {}

Wallpaper::~Wallpaper()
{
    if (m_image)
        g_object_unref(m_image);
}

bool Wallpaper::set(const WallpaperSettings& settings)
{
    if (settings == m_settings && (m_image || settings.path.empty()))
        return true;
    const bool newImage = settings.path != m_settings.path;
    m_settings = settings;
    m_texture.reset(); // redo the scaling
    if (!newImage)
        return true;
    if (m_image) {
        g_object_unref(m_image);
        m_image = nullptr;
    }
    if (settings.path.empty())
        return true;
    GError* error = nullptr;
    m_image = gdk_pixbuf_new_from_file(settings.path.c_str(), &error);
    if (!m_image) {
        log::error("desktop", "wallpaper {}: {}", settings.path, error ? error->message : "unreadable");
        if (error)
            g_error_free(error);
        return false;
    }
    log::info("desktop", "wallpaper {} ({}x{}, {})", settings.path, gdk_pixbuf_get_width(m_image),
              gdk_pixbuf_get_height(m_image), settings.mode);
    return true;
}

void Wallpaper::collect(Rect screen, std::vector<vk::TextureDraw>& draws)
{
    if (!m_image || screen.empty())
        return;
    const std::int32_t iw = gdk_pixbuf_get_width(m_image), ih = gdk_pixbuf_get_height(m_image);
    const std::string& mode = m_settings.mode;

    if (!m_texture || m_screen != screen || m_mode != mode) {
        // Size the image is drawn at, and where (relative to the screen).
        std::int32_t w = iw, h = ih;
        if (mode == "fill" || mode == "fit") {
            const double sx = double(screen.width) / iw, sy = double(screen.height) / ih;
            const double scale = mode == "fill" ? std::max(sx, sy) : std::min(sx, sy);
            w = std::max(1, std::int32_t(std::lround(iw * scale)));
            h = std::max(1, std::int32_t(std::lround(ih * scale)));
        } else if (mode == "stretch") {
            w = screen.width;
            h = screen.height;
        }
        m_placed = {screen.x + (screen.width - w) / 2, screen.y + (screen.height - h) / 2, w, h};

        // "fill" overflows the screen: only scale and upload the part that shows.
        GdkPixbuf* scaled = gdk_pixbuf_scale_simple(m_image, w, h, GDK_INTERP_HYPER);
        if (!scaled)
            return;
        Rect visible{0, 0, w, h};
        if (mode == "fill") {
            visible = {screen.x - m_placed.x, screen.y - m_placed.y, screen.width, screen.height};
            m_placed = screen;
        }

        // gdk-pixbuf: RGB(A), straight alpha. GPU: BGRA, premultiplied.
        const int channels = gdk_pixbuf_get_n_channels(scaled);
        const int stride = gdk_pixbuf_get_rowstride(scaled);
        const guchar* src = gdk_pixbuf_read_pixels(scaled);
        std::vector<std::uint8_t> bgra(std::size_t(visible.width) * visible.height * 4);
        for (std::int32_t y = 0; y < visible.height; ++y) {
            const guchar* row = src + std::size_t(y + visible.y) * stride + std::size_t(visible.x) * channels;
            std::uint8_t* out = &bgra[std::size_t(y) * visible.width * 4];
            for (std::int32_t x = 0; x < visible.width; ++x, row += channels, out += 4) {
                const unsigned a = channels == 4 ? row[3] : 255;
                out[0] = std::uint8_t(row[2] * a / 255);
                out[1] = std::uint8_t(row[1] * a / 255);
                out[2] = std::uint8_t(row[0] * a / 255);
                out[3] = std::uint8_t(a);
            }
        }
        g_object_unref(scaled);

        m_texture = m_renderer.createTexture(std::uint32_t(visible.width), std::uint32_t(visible.height),
                                             VK_FORMAT_B8G8R8A8_UNORM, false);
        if (!m_texture)
            return;
        const Rect all{0, 0, visible.width, visible.height};
        m_texture->write(bgra.data(), std::uint32_t(visible.width) * 4, {&all, 1});
        m_screen = screen;
        m_mode = mode;
    }

    if (mode == "tile") {
        for (std::int32_t y = screen.y; y < screen.y + screen.height; y += m_placed.height) {
            for (std::int32_t x = screen.x; x < screen.x + screen.width; x += m_placed.width)
                draws.push_back({.texture = m_texture.get(), .dst = {x, y, m_placed.width, m_placed.height}});
        }
        return;
    }
    draws.push_back({.texture = m_texture.get(), .dst = m_placed, .scissor = screen});
}

} // namespace dragon
