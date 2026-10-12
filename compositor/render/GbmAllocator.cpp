#include "render/GbmAllocator.h"

#include "core/Log.h"

#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <gbm.h>

namespace dragon {

GbmBuffer::~GbmBuffer()
{
    gbm_bo_destroy(m_bo);
}

std::unique_ptr<GbmAllocator> GbmAllocator::create(UniqueFd drmFd)
{
    gbm_device* device = gbm_create_device(drmFd.get());
    if (!device) {
        log::error("render", "gbm_create_device failed");
        return nullptr;
    }
    log::info("render", "GBM backend: {}", gbm_device_get_backend_name(device));
    return std::unique_ptr<GbmAllocator>(new GbmAllocator(std::move(drmFd), device));
}

GbmAllocator::~GbmAllocator()
{
    gbm_device_destroy(m_device);
}

std::unique_ptr<GbmBuffer> GbmAllocator::allocate(std::uint32_t width, std::uint32_t height, std::uint32_t format,
                                                  std::span<const std::uint64_t> modifiers, std::uint32_t usage)
{
    gbm_bo* bo = modifiers.empty()
        ? gbm_bo_create(m_device, width, height, format, usage)
        : gbm_bo_create_with_modifiers2(m_device, width, height, format, modifiers.data(),
                                        unsigned(modifiers.size()), usage);
    if (!bo) {
        log::error("render", "GBM allocation {}x{} format {:#010x} failed: {}", width, height, format,
                   std::strerror(errno));
        return nullptr;
    }

    DmaBuf buf{width, height, format, gbm_bo_get_modifier(bo), {}};
    const int planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < planes; ++i) {
        UniqueFd fd(gbm_bo_get_fd_for_plane(bo, i));
        if (!fd) {
            log::error("render", "gbm_bo_get_fd_for_plane({}) failed", i);
            gbm_bo_destroy(bo);
            return nullptr;
        }
        buf.planes.push_back({std::move(fd), gbm_bo_get_offset(bo, i), gbm_bo_get_stride_for_plane(bo, i)});
    }

    log::debug("render", "allocated {}x{} format {:#010x} modifier {:#018x}, {} plane(s)", width, height, format,
               buf.modifier, planes);
    return std::unique_ptr<GbmBuffer>(new GbmBuffer(bo, std::move(buf)));
}

} // namespace dragon
