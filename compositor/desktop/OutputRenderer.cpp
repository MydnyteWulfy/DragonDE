#include "desktop/OutputRenderer.h"

#include "Server.h"
#include "backend/Backend.h"
#include "core/Log.h"
#include "desktop/Desktop.h"
#include "desktop/SurfaceTextures.h"
#include "protocols/Compositor.h"
#include "protocols/Output.h"
#include "render/Gpu.h"

#include <chrono>

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

    // Everything we drew may draw its next frame now.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto msec = std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (Surface* s : m_drawn) {
        enter(s);
        s->sendFrameDone(msec);
    }
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
