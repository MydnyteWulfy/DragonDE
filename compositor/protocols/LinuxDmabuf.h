#pragma once

#include "core/UniqueFd.h"
#include "dragon-protocols/linux-dmabuf-v1.h"
#include "dragon-protocols/wayland.h"
#include "render/DmaBuf.h"
#include "wayland/Global.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <sys/types.h>
#include <vector>

namespace dragon {

class Server;

struct DmabufFormat {
    std::uint32_t format;   // DRM fourcc
    std::uint64_t modifier; // DRM_FORMAT_MOD_*
};

// What the renderer can import, handed to the protocol layer so it stays GPU-agnostic.
struct DmabufSupport {
    dev_t mainDevice = 0; // the render node clients should allocate on
    std::vector<DmabufFormat> formats;
    // Tries the import for real (e.g. into Vulkan); false rejects the buffer.
    std::function<bool(const DmaBuf&)> validate;
};

// A wl_buffer backed by a client's dma-buf.
class DmabufBuffer final : public proto::WlBuffer {
public:
    DmabufBuffer(wl_client* client, std::uint32_t id, DmaBuf buffer)
        : WlBuffer(client, 1u, id), m_buffer(std::move(buffer)) {}

    // Null for buffers that aren't ours (e.g. wl_shm).
    static DmabufBuffer* from(wl_resource* resource)
    {
        return static_cast<DmabufBuffer*>(proto::WlBuffer::fromResource(resource));
    }

    const DmaBuf& dmabuf() const { return m_buffer; }

private:
    DmaBuf m_buffer;
};

// The zwp_linux_dmabuf_v1 global: format/modifier advertisement (a sealed
// format table + feedback, v4) and buffer creation.
class LinuxDmabuf {
public:
    LinuxDmabuf(Server& server, DmabufSupport support);
    ~LinuxDmabuf();
    LinuxDmabuf(const LinuxDmabuf&) = delete;
    LinuxDmabuf& operator=(const LinuxDmabuf&) = delete;

    const DmabufSupport& support() const { return m_support; }
    bool supportsFormat(std::uint32_t format) const;
    bool supports(std::uint32_t format, std::uint64_t modifier) const;

    void sendFeedback(proto::ZwpLinuxDmabufFeedbackV1& feedback) const;

private:
    DmabufSupport m_support;
    UniqueFd m_formatTable;
    std::uint32_t m_formatTableSize = 0;
    std::unique_ptr<Global> m_global;
};

} // namespace dragon
