#pragma once

#include "core/Signal.h"

#include <memory>
#include <unordered_map>

namespace dragon {

class Gpu;
class Server;
class Surface;
namespace vk {
class VulkanTexture;
}

// Keeps a GPU texture for every surface with content. wl_shm buffers are
// copied on commit (only the damaged part) and released to the client
// immediately; dma-buf import arrives in M1 stage 3.
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
    void track(Surface* surface);
    void update(Surface* surface, Entry& entry);

    Gpu& m_gpu;
    std::unordered_map<Surface*, std::unique_ptr<Entry>> m_entries;
    Connection m_surfaceCreated;
};

} // namespace dragon
