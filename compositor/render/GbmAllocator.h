#pragma once

#include "core/UniqueFd.h"
#include "render/DmaBuf.h"

#include <cstdint>
#include <memory>
#include <span>

struct gbm_bo;
struct gbm_device;

namespace dragon {

// A buffer allocated by GBM, exported as a dma-buf.
class GbmBuffer {
public:
    ~GbmBuffer();
    GbmBuffer(const GbmBuffer&) = delete;
    GbmBuffer& operator=(const GbmBuffer&) = delete;

    gbm_bo* bo() const { return m_bo; }
    const DmaBuf& dmabuf() const { return m_dmabuf; }

private:
    friend class GbmAllocator;
    GbmBuffer(gbm_bo* bo, DmaBuf dmabuf) : m_bo(bo), m_dmabuf(std::move(dmabuf)) {}

    gbm_bo* m_bo;
    DmaBuf m_dmabuf;
};

// Allocates render targets. We render into GBM buffers in every backend:
// KMS scans them out, the nested backend hands them to the parent compositor.
class GbmAllocator {
public:
    static std::unique_ptr<GbmAllocator> create(UniqueFd drmFd);
    ~GbmAllocator();
    GbmAllocator(const GbmAllocator&) = delete;
    GbmAllocator& operator=(const GbmAllocator&) = delete;

    // `modifiers` is the acceptable set (GBM/the driver picks the best one);
    // empty means "driver's choice, implicit modifier". `usage` is GBM_BO_USE_*.
    std::unique_ptr<GbmBuffer> allocate(std::uint32_t width, std::uint32_t height, std::uint32_t format,
                                        std::span<const std::uint64_t> modifiers, std::uint32_t usage);

    gbm_device* device() const { return m_device; }

private:
    GbmAllocator(UniqueFd fd, gbm_device* device) : m_fd(std::move(fd)), m_device(device) {}

    UniqueFd m_fd;
    gbm_device* m_device;
};

} // namespace dragon
