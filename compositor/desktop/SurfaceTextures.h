#pragma once

#include "core/Signal.h"

#include <memory>
#include <unordered_map>

namespace dragon {

class ClientBuffer;
class Gpu;
class Server;
class Surface;
namespace vk {
class VulkanTexture;
}

// Keeps a GPU texture for every surface with content. wl_shm buffers are
// copied on commit (only the damaged part) and released to the client
// immediately. dma-bufs are imported zero-copy, once per wl_buffer (clients
// cycle through a few), and stay locked while on screen.
class SurfaceTextures {
public:
    SurfaceTextures(Server& server, Gpu& gpu);
    ~SurfaceTextures();
    SurfaceTextures(const SurfaceTextures&) = delete;
    SurfaceTextures& operator=(const SurfaceTextures&) = delete;

    // Null until the surface's first buffer was uploaded.
    vk::VulkanTexture* get(Surface* surface) const;

private:
    struct Entry;
    struct Imported;
    void track(Surface* surface);
    void update(Surface* surface, Entry& entry);
    void updateShm(Surface* surface, Entry& entry, ClientBuffer* buffer);
    void updateDmabuf(Entry& entry, ClientBuffer* buffer);

    Gpu& m_gpu;
    std::unordered_map<Surface*, std::unique_ptr<Entry>> m_entries;
    std::unordered_map<ClientBuffer*, std::unique_ptr<Imported>> m_imported;
    Connection m_surfaceCreated;
};

} // namespace dragon
