#include "desktop/OutputRenderer.h"

#include "Server.h"
#include "backend/Backend.h"
#include "core/Log.h"
#include "core/Png.h"
#include "desktop/Desktop.h"
#include "desktop/SurfaceTextures.h"
#include "protocols/Compositor.h"
#include "protocols/Output.h"
#include "render/Gpu.h"
#include "render/vulkan/VulkanImage.h"

#include <chrono>
#include <drm_fourcc.h>
#include <gbm.h>
#include <utility>
#include <vector>

namespace dragon {

OutputRenderer::OutputRenderer(Server& server, Gpu& gpu, Desktop& desktop, SurfaceTextures& textures, Output& output)
    : m_server(server), m_gpu(gpu), m_desktop(desktop), m_textures(textures), m_output(output)
{
    m_frame = output.frame.connect([this] { render(); });
}

OutputRenderer::~OutputRenderer() = default;

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
    for (const auto& item : m_desktop.renderList())
        collect(item.surface, item.origin);

    if (!m_gpu.renderer().render(*slot->image, kBackground, m_draws)) {
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
    if (!image || !m_gpu.renderer().render(*image, kBackground, m_draws) || !m_gpu.renderer().waitIdle()) {
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

void OutputRenderer::collect(Surface* surface, Point origin)
{
    for (Surface* entry : surface->stack()) {
        if (entry == surface) {
            if (!surface->hasContent())
                continue;
            m_drawn.push_back(surface);
            if (auto* tex = m_textures.get(surface)) {
                // Transforms other than normal come with output scaling work.
                const Size size = surface->size();
                m_draws.push_back({tex, {origin.x, origin.y, size.width, size.height}});
            }
        } else if (auto* sub = entry->asSubsurface(); sub && entry->hasContent()) {
            collect(entry, origin + sub->position());
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
