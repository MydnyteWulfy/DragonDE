#include "desktop/OutputRenderer.h"

#include "Server.h"
#include "backend/Backend.h"
#include "core/Log.h"
#include "core/Png.h"
#include "desktop/Desktop.h"
#include "desktop/SurfaceTextures.h"
#include "desktop/Titlebar.h"
#include "desktop/Toplevel.h"
#include "desktop/Wallpaper.h"
#include "protocols/LayerShell.h"
#include "protocols/Compositor.h"
#include "protocols/Output.h"
#include "render/Gpu.h"
#include "render/vulkan/VulkanImage.h"
#include "render/vulkan/VulkanTexture.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <drm_fourcc.h>
#include <gbm.h>
#include <utility>
#include <vector>

namespace dragon {

OutputRenderer::OutputRenderer(Server& server, Gpu& gpu, Desktop& desktop, SurfaceTextures& textures, Output& output)
    : m_server(server), m_gpu(gpu), m_desktop(desktop), m_textures(textures), m_output(output),
      m_titlebars(std::make_unique<TitlebarRenderer>(gpu.renderer())),
      m_wallpaper(std::make_unique<Wallpaper>(gpu.renderer()))
{
    m_frame = output.frame.connect([this] { render(); });
}

OutputRenderer::~OutputRenderer() = default;

void OutputRenderer::setWallpaper(const WallpaperSettings& settings)
{
    m_wallpaper->set(settings);
}

void OutputRenderer::render()
{
    Swapchain* swapchain = m_output.swapchain();
    if (!swapchain)
        return;
    SwapchainSlot* slot = swapchain->acquire();
    if (!slot) {
        log::debug("render", "{}: every buffer is still on screen, skipping a frame", m_output.name());
        return;
    }

    m_draws.clear();
    m_drawn.clear();
    m_captureAt.reset();
    m_captured = false;
    const auto& mode = m_output.mode();
    m_wallpaper->collect({0, 0, mode.width, mode.height}, m_draws);
    for (const auto& item : m_desktop.renderList()) {
        if (!item.belowWindows && !m_captureAt)
            m_captureAt = m_draws.size();
        if (item.window)
            collectWindow(item);
        else if (item.layer && item.layerStyle)
            collectLayer(item);
        else
            collect(item.surface, item.origin);
    }
    m_titlebars->sweep();
    for (const auto& overlay : m_overlays) {
        if (auto item = overlay())
            collect(item->surface, item->origin);
    }

    if (!m_gpu.renderer().render(*slot->image, m_background, m_draws)) {
        Swapchain::release(*slot);
        return;
    }
    m_output.present(*slot);
    if (!m_screenshotPath.empty())
        saveScreenshot();

    // Everything we drew may draw its next frame now.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto msec = std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (Surface* s : m_drawn) {
        enter(s);
        s->sendFrameDone(msec);
    }
}

void OutputRenderer::saveScreenshot()
{
    const std::string path = std::exchange(m_screenshotPath, {});
    const auto& mode = m_output.mode();
    const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    auto buffer = m_gpu.allocator().allocate(std::uint32_t(mode.width), std::uint32_t(mode.height),
                                             DRM_FORMAT_XRGB8888, {&linear, 1}, GBM_BO_USE_RENDERING);
    auto image = buffer ? vk::VulkanImage::importDmaBuf(m_gpu.vulkan(), buffer->dmabuf(),
                                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "screenshot")
                        : nullptr;
    if (!image || !m_gpu.renderer().render(*image, m_background, m_draws) || !m_gpu.renderer().waitIdle()) {
        log::error("render", "screenshot failed");
        return;
    }

    std::uint32_t stride = 0;
    void* mapData = nullptr;
    const auto w = std::uint32_t(mode.width), h = std::uint32_t(mode.height);
    auto* base = static_cast<const std::uint8_t*>(
        gbm_bo_map(buffer->bo(), 0, 0, w, h, GBM_BO_TRANSFER_READ, &stride, &mapData));
    if (!base) {
        log::error("render", "screenshot: cannot map buffer");
        return;
    }
    std::vector<std::uint8_t> rgb(std::size_t(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t* px = base + std::size_t(y) * stride + std::size_t(x) * 4; // B G R X
            std::uint8_t* out = &rgb[(std::size_t(y) * w + x) * 3];
            out[0] = px[2], out[1] = px[1], out[2] = px[0];
        }
    }
    gbm_bo_unmap(buffer->bo(), mapData);

    if (png::writeRgb(path, w, h, rgb))
        log::info("render", "screenshot saved to {}", path);
    else
        log::error("render", "cannot write {}", path);
}

namespace {
std::array<float, 4> rgba(const Color& c, float alpha = 1.0f)
{
    return {c.r, c.g, c.b, c.a * alpha};
}
} // namespace

void OutputRenderer::addBackdrop(const vk::RoundedRect& region, const BlurStyle& blur, float alpha,
                                 vk::VulkanTexture* mask, Rect maskRect, float maskThreshold)
{
    const vk::BlurParams params{.size = blur.size,
                                .passes = blur.passes,
                                .noise = float(blur.noise),
                                .contrast = float(blur.contrast),
                                .brightness = float(blur.brightness),
                                .vibrancy = float(blur.vibrancy)};
    vk::TextureDraw d{.alpha = alpha};
    d.backdrop = vk::BackdropDraw{.region = region,
                                  .params = params,
                                  .fromCapture = blur.xray,
                                  .mask = mask,
                                  .maskRect = maskRect,
                                  .maskThreshold = maskThreshold};
    if (blur.xray && !m_captured) {
        // One blur of everything below the windows, shared by every xray backdrop.
        vk::TextureDraw capture{.captureBackdrop = true};
        capture.backdrop = vk::BackdropDraw{.params = params};
        const std::size_t at = std::min(m_captureAt.value_or(m_draws.size()), m_draws.size());
        m_draws.insert(m_draws.begin() + std::ptrdiff_t(at), capture);
        m_captured = true;
    }
    m_draws.push_back(d);
}

void OutputRenderer::collectWindow(const Desktop::RenderItem& item)
{
    const Desktop::Window& w = *item.window;
    const Desktop::Frame f = m_desktop.frameOf(w);
    const WindowStyle& style = w.style;
    if (!f.styled) {
        const auto alpha = float(w.toplevel->fullscreen() ? style.fullscreenOpacity : 1.0);
        collect(item.surface, item.origin, {.alpha = alpha});
        return;
    }
    const bool active = m_desktop.focusedWindow() == &w;
    const auto alpha = float(active ? style.activeOpacity : style.inactiveOpacity);
    const vk::RoundedRect shape{f.outer, f.radius};
    const vk::RoundedRect withBorder{f.outer.adjusted(f.border), f.radius > 0 ? f.radius + float(f.border) : 0};

    if (f.shadow) {
        const auto& sh = style.shadow;
        const Rect cast{withBorder.rect.x + sh.offsetX, withBorder.rect.y + sh.offsetY, withBorder.rect.width,
                        withBorder.rect.height};
        vk::TextureDraw d{.dst = cast.adjusted(sh.size), .alpha = alpha, .clip = withBorder, .invertClip = true};
        d.shape = {.kind = vk::ShapeDraw::Kind::Shadow,
                   .rect = {cast, withBorder.radius},
                   .size = float(sh.size),
                   .color = rgba(active ? sh.color : sh.inactiveColor.value_or(sh.color)),
                   .angle = float(sh.power)};
        m_draws.push_back(d);
    }
    if (style.glow.enabled && !w.toplevel->maximized()) {
        const auto& g = style.glow;
        const Color& color = active ? g.active : g.inactive;
        if (color.a > 0) {
            vk::TextureDraw d{.dst = withBorder.rect.adjusted(g.size), .alpha = alpha, .clip = withBorder,
                              .invertClip = true};
            d.shape = {.kind = vk::ShapeDraw::Kind::Shadow,
                       .rect = withBorder,
                       .size = float(g.size),
                       .color = rgba(color),
                       .angle = float(g.power)};
            m_draws.push_back(d);
        }
    }
    // Blur shows only through what's see-through: skip it for opaque windows.
    if (style.blur.enabled) {
        bool clear = alpha < 1.0f;
        if (!f.titlebar.empty())
            clear = clear || (active ? style.titlebar.active : style.titlebar.inactive).background.a < 1.0f;
        if (!clear) {
            const auto* tex = m_textures.get(item.surface);
            clear = !tex || !(tex->opaque() || item.surface->state().opaque.covers(w.toplevel->geometry()));
        }
        if (clear)
            addBackdrop(withBorder, style.blur, 1.0f);
    }
    if (f.border > 0) {
        const Paint& paint = active ? style.border.active : style.border.inactive;
        vk::TextureDraw d{.dst = withBorder.rect, .alpha = alpha};
        d.shape = {.kind = vk::ShapeDraw::Kind::Border,
                   .rect = shape,
                   .size = float(f.border),
                   .color = rgba(paint.color),
                   .color2 = rgba(paint.end.value_or(paint.color)),
                   .angle = float(style.border.gradientAngle * std::numbers::pi / 180.0)};
        m_draws.push_back(d);
    }
    if (!f.titlebar.empty()) {
        const TitlebarRenderer::State state{.width = f.titlebar.width,
                                            .title = w.toplevel->title(),
                                            .active = active,
                                            .maximized = w.toplevel->maximized(),
                                            .hovered = w.hoveredButton,
                                            .pressed = w.pressedButton,
                                            .style = style.titlebar};
        if (auto* tex = m_titlebars->get(&w, state))
            m_draws.push_back({.texture = tex, .dst = f.titlebar, .alpha = alpha, .clip = shape});
    }
    // The client's content, cut to its geometry (no client-side shadows) and our shape.
    collect(item.surface, item.origin, {.alpha = alpha, .scissor = f.content, .clip = shape});

    if (!active && style.dim.strength > 0) {
        Color dim = style.dim.color;
        dim.a = float(style.dim.strength);
        vk::TextureDraw d{.dst = f.outer, .alpha = alpha};
        d.shape = {.kind = vk::ShapeDraw::Kind::Fill, .rect = shape, .color = rgba(dim)};
        m_draws.push_back(d);
    }
}

void OutputRenderer::collectLayer(const Desktop::RenderItem& item)
{
    const LayerStyle& style = *item.layerStyle;
    const Size size = item.surface->size();
    const Rect r{item.origin.x, item.origin.y, size.width, size.height};
    const vk::RoundedRect shape{r, float(style.cornerRadius)};
    if (style.shadow.enabled) {
        vk::TextureDraw d{.dst = r.adjusted(style.shadow.size), .clip = shape, .invertClip = true};
        d.shape = {.kind = vk::ShapeDraw::Kind::Shadow,
                   .rect = shape,
                   .size = float(style.shadow.size),
                   .color = rgba(style.shadow.color),
                   .angle = float(style.shadow.power)};
        m_draws.push_back(d);
    }
    if (style.blur.enabled) {
        auto* tex = style.ignoreAlpha > 0 ? m_textures.get(item.surface) : nullptr;
        addBackdrop(shape, style.blur, 1.0f, tex, r, float(style.ignoreAlpha));
    }
    SurfaceStyle s{.alpha = float(style.opacity)};
    if (style.cornerRadius > 0)
        s.clip = shape;
    collect(item.surface, item.origin, s);
}

void OutputRenderer::collect(Surface* surface, Point origin)
{
    collect(surface, origin, SurfaceStyle{});
}

void OutputRenderer::collect(Surface* surface, Point origin, const SurfaceStyle& style)
{
    for (Surface* entry : surface->stack()) {
        if (entry == surface) {
            if (!surface->hasContent())
                continue;
            m_drawn.push_back(surface);
            if (auto* tex = m_textures.get(surface)) {
                // Transforms other than normal come with output scaling work.
                const Size size = surface->size();
                m_draws.push_back({.texture = tex,
                                   .dst = {origin.x, origin.y, size.width, size.height},
                                   .alpha = style.alpha,
                                   .scissor = style.scissor,
                                   .clip = style.clip});
            }
        } else if (auto* sub = entry->asSubsurface(); sub && entry->hasContent()) {
            collect(entry, origin + sub->position(), style);
        }
    }
}

void OutputRenderer::enter(Surface* surface)
{
    if (m_entered.contains(surface))
        return;
    OutputGlobal* global = m_server.outputGlobal(m_output);
    if (!global)
        return;
    const auto resources = global->resourcesFor(surface->client());
    if (resources.empty())
        return; // client hasn't bound wl_output (yet)
    for (wl_resource* r : resources)
        surface->sendEnter(r);
    m_entered.emplace(surface, surface->destroyed.connect([this, surface] { m_entered.erase(surface); }));
}

} // namespace dragon
