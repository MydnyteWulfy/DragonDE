#include "render/DmaBuf.h"

#include "core/Log.h"

#include <cerrno>
#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace dragon::dmabuf {

namespace {
std::uint32_t syncFlags(Access access)
{
    return access == Access::Write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
}

int ioctlRetry(int fd, unsigned long request, void* arg)
{
    int ret;
    do {
        ret = ::ioctl(fd, request, arg);
    } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
    return ret;
}
} // namespace

UniqueFd exportSyncFile(int dmabufFd, Access access)
{
    dma_buf_export_sync_file data{.flags = syncFlags(access), .fd = -1};
    if (ioctlRetry(dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data) != 0) {
        log::warn("render", "DMA_BUF_IOCTL_EXPORT_SYNC_FILE failed: {}", std::strerror(errno));
        return {};
    }
    return UniqueFd(data.fd);
}

bool importSyncFile(int dmabufFd, int syncFile, Access access)
{
    dma_buf_import_sync_file data{.flags = syncFlags(access), .fd = syncFile};
    if (ioctlRetry(dmabufFd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) != 0) {
        log::warn("render", "DMA_BUF_IOCTL_IMPORT_SYNC_FILE failed: {}", std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace dragon::dmabuf
