#include "desktop/Titlebar.h"

#include "core/Log.h"
#include "render/vulkan/VulkanRenderer.h"
#include "render/vulkan/VulkanTexture.h"

#include <cairo.h>
#include <cmath>
#include <numbers>
#include <pango/pangocairo.h>

namespace dragon {

namespace {

void setColor(cairo_t* cr, const Color& c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

// The icon of a button, centered in `r`, `size` across.
void drawIcon(cairo_t* cr, const std::string& action, const Rect& r, bool maximized)
{
    const double cx = r.x + r.width / 2.0, cy = r.y + r.height / 2.0;
    const double s = std::round(r.width * 0.28); // half the icon's extent
    cairo_set_line_width(cr, std::max(1.0, r.width / 12.0));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (action == "minimize") {
        cairo_move_to(cr, cx - s, cy + 0.5);
        cairo_line_to(cr, cx + s, cy + 0.5);
    } else if (action == "maximize") {
        if (maximized) { // "restore": two overlapping squares
            const double d = s * 0.35;
            cairo_rectangle(cr, cx - s + 0.5, cy - s + 2 * d + 0.5, 2 * s - 2 * d, 2 * s - 2 * d);
            cairo_move_to(cr, cx - s + 2 * d + 0.5, cy - s + 2 * d);
            cairo_line_to(cr, cx - s + 2 * d + 0.5, cy - s + 0.5);
            cairo_line_to(cr, cx + s + 0.5, cy - s + 0.5);
            cairo_line_to(cr, cx + s + 0.5, cy + s - 2 * d + 0.5);
            cairo_line_to(cr, cx + s - 2 * d, cy + s - 2 * d + 0.5);
        } else {
            cairo_rectangle(cr, cx - s + 0.5, cy - s + 0.5, 2 * s, 2 * s);
        }
    } else if (action == "close") {
        cairo_move_to(cr, cx - s, cy - s);
        cairo_line_to(cr, cx + s, cy + s);
        cairo_move_to(cr, cx + s, cy - s);
        cairo_line_to(cr, cx - s, cy + s);
    }
    cairo_stroke(cr);
}

void drawButton(cairo_t* cr, const TitlebarLayout::Button& b, const TitlebarColors& c, const TitlebarStyle& style,
                bool hovered, bool pressed, bool maximized)
{
    const Rect& r = b.rect;
    const double cx = r.x + r.width / 2.0, cy = r.y + r.height / 2.0, radius = r.width / 2.0;
    const bool close = b.action == "close";
    if (style.buttonStyle == "circle") {
        // Colored dots; the icon shows on hover.
        setColor(cr, close ? c.closeHover : c.button);
        cairo_arc(cr, cx, cy, radius * (pressed ? 0.85 : 1.0), 0, 2 * std::numbers::pi);
        cairo_fill(cr);
        if (hovered || pressed) {
            setColor(cr, c.background);
            drawIcon(cr, b.action, r, maximized);
        }
        return;
    }
    if (hovered || pressed) {
        Color bg = close ? c.closeHover : c.buttonHover;
        if (pressed)
            bg.a *= 0.75f;
        setColor(cr, bg);
        cairo_arc(cr, cx, cy, radius, 0, 2 * std::numbers::pi);
        cairo_fill(cr);
    }
    setColor(cr, close && (hovered || pressed) ? Color{1, 1, 1, 1} : c.button);
    drawIcon(cr, b.action, r, maximized);
}

void drawTitle(cairo_t* cr, const TitlebarRenderer::State& s, const Rect& area, const TitlebarColors& c)
{
    if (s.title.empty() || area.width <= 0)
        return;
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_from_string(s.style.font.c_str());
    pango_font_description_set_size(font, gint(s.style.fontSize * PANGO_SCALE));
    if (s.style.bold)
        pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    pango_layout_set_text(layout, s.title.c_str(), int(s.title.size()));
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_width(layout, area.width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, s.style.align == "left"    ? PANGO_ALIGN_LEFT
                                       : s.style.align == "right" ? PANGO_ALIGN_RIGHT
                                                                  : PANGO_ALIGN_CENTER);
    int textWidth = 0, textHeight = 0;
    pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
    setColor(cr, c.text);
    cairo_move_to(cr, area.x, area.y + (area.height - textHeight) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

} // namespace

struct TitlebarRenderer::Entry {
    State state;
    std::unique_ptr<vk::VulkanTexture> texture;
    bool used = true;
};

TitlebarRenderer::TitlebarRenderer(vk::VulkanRenderer& renderer) : m_renderer(renderer) {}

TitlebarRenderer::~TitlebarRenderer() = default;

vk::VulkanTexture* TitlebarRenderer::get(const void* key, const State& s)
{
    const std::int32_t height = s.style.height;
    if (s.width <= 0 || height <= 0)
        return nullptr;
    auto& entry = m_entries[key];
    if (!entry)
        entry = std::make_unique<Entry>();
    entry->used = true;
    if (entry->texture && entry->state == s)
        return entry->texture.get();

    if (!entry->texture || entry->texture->width() != std::uint32_t(s.width) ||
        entry->texture->height() != std::uint32_t(height)) {
        entry->texture =
            m_renderer.createTexture(std::uint32_t(s.width), std::uint32_t(height), VK_FORMAT_B8G8R8A8_UNORM, false);
        if (!entry->texture)
            return nullptr;
    }
    entry->state = s;

    // ARGB32 is premultiplied BGRA in memory on little-endian: what the GPU samples.
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s.width, height);
    cairo_t* cr = cairo_create(surface);
    const TitlebarColors& c = s.active ? s.style.active : s.style.inactive;
    setColor(cr, c.background);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const auto layout = layoutTitlebar(s.style, s.width);
    for (std::size_t i = 0; i < layout.buttons.size(); ++i)
        drawButton(cr, layout.buttons[i], c, s.style, int(i) == s.hovered, int(i) == s.pressed, s.maximized);
    drawTitle(cr, s, layout.text, c);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const Rect all{0, 0, s.width, height};
    entry->texture->write(cairo_image_surface_get_data(surface), std::uint32_t(cairo_image_surface_get_stride(surface)),
                          {&all, 1});
    cairo_surface_destroy(surface);
    return entry->texture.get();
}

void TitlebarRenderer::sweep()
{
    std::erase_if(m_entries, [](const auto& e) { return !e.second->used; });
    for (auto& [key, entry] : m_entries)
        entry->used = false;
}

} // namespace dragon
