#include "desktop/SurfaceTextures.h"

#include "Server.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "render/DmaBuf.h"
#include "render/Gpu.h"
#include "render/vulkan/VulkanTexture.h"

#include <drm_fourcc.h>
#include <optional>
#include <vector>
#include <wayland-server-core.h>

namespace dragon {

namespace {

struct ShmFormat {
    VkFormat format;
    bool opaque;
};

// 4-byte formats only: VulkanTexture copies 4 bytes per pixel.
std::optional<ShmFormat> shmFormat(std::uint32_t drmFormat)
{
    switch (drmFormat) {
    case DRM_FORMAT_ARGB8888: return ShmFormat{VK_FORMAT_B8G8R8A8_UNORM, false};
    case DRM_FORMAT_XRGB8888: return ShmFormat{VK_FORMAT_B8G8R8A8_UNORM, true};
    case DRM_FORMAT_ABGR8888: return ShmFormat{VK_FORMAT_R8G8B8A8_UNORM, false};
    case DRM_FORMAT_XBGR8888: return ShmFormat{VK_FORMAT_R8G8B8A8_UNORM, true};
    default: return std::nullopt;
    }
}

} // namespace

struct SurfaceTextures::Entry {
    vk::VulkanTexture* current = nullptr; // what to draw: `shm`, or an imported dma-buf
    std::unique_ptr<vk::VulkanTexture> shm;
    Connection committed;
    Connection dying;
};

struct SurfaceTextures::Imported {
    std::unique_ptr<vk::VulkanTexture> texture;
    Connection gone;
};

SurfaceTextures::SurfaceTextures(Server& server, Gpu& gpu) : m_gpu(gpu)
{
    m_surfaceCreated = server.surfaceCreated.connect([this](Surface* s) { track(s); });
}

SurfaceTextures::~SurfaceTextures()
{
    // Entries first: their connections must not fire into a half-destroyed cache.
    m_entries.clear();
    m_imported.clear();
}

vk::VulkanTexture* SurfaceTextures::get(Surface* surface) const
{
    auto it = m_entries.find(surface);
    return it == m_entries.end() ? nullptr : it->second->current;
}

void SurfaceTextures::track(Surface* surface)
{
    auto entry = std::make_unique<Entry>();
    auto* raw = entry.get();
    raw->committed = surface->committed.connect([this, surface, raw] { update(surface, *raw); });
    raw->dying = surface->aboutToBeDestroyed.connect([this, surface] { m_entries.erase(surface); });
    m_entries.emplace(surface, std::move(entry));
}

void SurfaceTextures::update(Surface* surface, Entry& entry)
{
    if (!surface->hasContent()) {
        entry.current = nullptr;
        entry.shm.reset();
        return;
    }
    ClientBuffer* buffer = surface->buffer();
    if (!buffer)
        return; // shm already uploaded and released; nothing new was attached
    if (buffer->kind() == ClientBuffer::Kind::DmaBuf)
        updateDmabuf(entry, buffer);
    else
        updateShm(surface, entry, buffer);
    surface->clearDamage();
}

void SurfaceTextures::updateDmabuf(Entry& entry, ClientBuffer* buffer)
{
    auto it = m_imported.find(buffer);
    if (it == m_imported.end()) {
        const DmaBuf* dmabuf = buffer->dmabuf();
        if (!dmabuf)
            return;
        auto tex = m_gpu.renderer().importTexture(*dmabuf);
        if (!tex) {
            log::warn("render", "cannot import client dma-buf {}x{} {:#010x}/{:#018x}", dmabuf->width,
                      dmabuf->height, dmabuf->format, dmabuf->modifier);
            return;
        }
        auto imported = std::make_unique<Imported>();
        imported->texture = std::move(tex);
        imported->gone = buffer->destroyed.connect([this, buffer] {
            auto* texture = m_imported.at(buffer)->texture.get();
            for (auto& [s, e] : m_entries) {
                if (e->current == texture)
                    e->current = nullptr;
            }
            m_imported.erase(buffer);
        });
        it = m_imported.emplace(buffer, std::move(imported)).first;
    }
    entry.current = it->second->texture.get();
    entry.shm.reset();
    // The buffer stays locked while current: we sample it every frame.
}

void SurfaceTextures::updateShm(Surface* surface, Entry& entry, ClientBuffer* buffer)
{
    wl_shm_buffer* shm = buffer->shmBuffer();
    const auto format = shmFormat(buffer->format());
    if (!shm || !format) {
        log::warn("render", "unsupported shm format {:#010x}", buffer->format());
        surface->releaseBuffer();
        return;
    }

    const Size size = buffer->size();
    auto& tex = entry.shm;
    const bool recreate = !tex || tex->width() != std::uint32_t(size.width) ||
                          tex->height() != std::uint32_t(size.height) || tex->format() != format->format ||
                          tex->opaque() != format->opaque;
    if (recreate) {
        tex = m_gpu.renderer().createTexture(std::uint32_t(size.width), std::uint32_t(size.height), format->format,
                                             format->opaque);
        if (!tex) {
            entry.current = nullptr;
            surface->releaseBuffer();
            return;
        }
    }
    entry.current = tex.get();

    // Copy only what changed. Surface-coordinate damage would need the
    // scale/transform mapped back to the buffer, so it falls back to a full copy.
    const auto& state = surface->state();
    std::vector<Rect> rects;
    if (recreate || !state.surfaceDamage.empty() || state.bufferDamage.empty())
        rects.push_back({0, 0, size.width, size.height});
    else
        rects = state.bufferDamage.rects();

    wl_shm_buffer_begin_access(shm);
    tex->write(wl_shm_buffer_get_data(shm), std::uint32_t(wl_shm_buffer_get_stride(shm)), rects);
    wl_shm_buffer_end_access(shm);

    surface->releaseBuffer(); // the client may reuse it right away
}

} // namespace dragon
