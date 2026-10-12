#include "render/RenderNode.h"

#include "core/Log.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <xf86drm.h>

namespace dragon {

namespace {
bool deviceHasNode(drmDevicePtr device, dev_t node)
{
    for (int type = 0; type < DRM_NODE_MAX; ++type) {
        struct stat st {};
        if ((device->available_nodes & (1 << type)) && ::stat(device->nodes[type], &st) == 0 &&
            st.st_rdev == node)
            return true;
    }
    return false;
}
} // namespace

std::optional<RenderNode> openRenderNode(std::optional<dev_t> preferred)
{
    drmDevicePtr devices[16];
    const int count = drmGetDevices2(0, devices, 16);
    if (count <= 0) {
        log::error("render", "no DRM devices found");
        return std::nullopt;
    }

    std::optional<RenderNode> result;
    for (int i = 0; i < count && !result; ++i) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;
        const char* path = devices[i]->nodes[DRM_NODE_RENDER];

        struct stat st {};
        if (::stat(path, &st) != 0)
            continue;
        // Compositors may name their GPU by the primary node (card*) rather
        // than the render node, so match any node of the device.
        if (preferred && !deviceHasNode(devices[i], *preferred))
            continue;

        UniqueFd fd(::open(path, O_RDWR | O_CLOEXEC));
        if (!fd) {
            log::warn("render", "cannot open {}: {}", path, std::strerror(errno));
            continue;
        }
        result = RenderNode{std::move(fd), st.st_rdev, path};
    }
    drmFreeDevices(devices, count);

    if (result)
        log::info("render", "using render node {}", result->path);
    else
        log::error("render", "no usable render node{}", preferred ? " matching the requested device" : "");
    return result;
}

} // namespace dragon
