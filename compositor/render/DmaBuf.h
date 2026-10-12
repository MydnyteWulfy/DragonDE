#pragma once

#include "core/UniqueFd.h"

#include <cstdint>
#include <vector>

namespace dragon {

struct DmaBufPlane {
    UniqueFd fd;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

// A GPU buffer shared through the kernel's dma-buf mechanism: what GBM
// allocates, what Vulkan imports, what KMS scans out and what clients send
// through linux-dmabuf-v1. Owns its plane fds.
struct DmaBuf {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;   // DRM fourcc (DRM_FORMAT_*)
    std::uint64_t modifier = 0; // DRM_FORMAT_MOD_* (tiling/compression layout)
    std::vector<DmaBufPlane> planes;
};

// Implicit-sync bridge (kernel 6.0+). A dma-buf carries fences from whoever
// last used it; these move them in and out as sync_file fds that Vulkan can
// wait on or signal.
namespace dmabuf {

enum class Access { Read, Write };

// A sync_file that signals when it's safe to perform `access` on the buffer
// (Write waits for all readers and writers, Read only for writers).
// Invalid fd on failure or if nothing is pending.
UniqueFd exportSyncFile(int dmabufFd, Access access);

// Attaches `syncFile` to the buffer, so later implicit-sync users wait for it.
bool importSyncFile(int dmabufFd, int syncFile, Access access);

} // namespace dmabuf
} // namespace dragon
