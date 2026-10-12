#include "protocols/Buffer.h"

#include <drm_fourcc.h>
#include <wayland-server-protocol.h>

namespace dragon {

namespace {
// wl_shm uses DRM fourccs, except for the two formats every compositor must support.
std::uint32_t shmFormatToDrm(std::uint32_t format)
{
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
    case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
    default: return format;
    }
}
} // namespace

ClientBuffer* ClientBuffer::get(wl_resource* buffer)
{
    if (!buffer)
        return nullptr;
    if (auto* l = wl_resource_get_destroy_listener(buffer, handleResourceDestroy))
        return reinterpret_cast<Listener*>(l)->owner;

    if (auto* shm = wl_shm_buffer_get(buffer)) {
        return new ClientBuffer(buffer, Kind::Shm, {wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm)},
                                shmFormatToDrm(wl_shm_buffer_get_format(shm)));
    }
    return nullptr;
}

ClientBuffer::ClientBuffer(wl_resource* resource, Kind kind, Size size, std::uint32_t format)
    : m_resource(resource), m_kind(kind), m_size(size), m_format(format)
{
    m_destroyListener.owner = this;
    m_destroyListener.listener.notify = handleResourceDestroy;
    wl_resource_add_destroy_listener(resource, &m_destroyListener.listener);
}

wl_shm_buffer* ClientBuffer::shmBuffer() const
{
    return m_kind == Kind::Shm && m_resource ? wl_shm_buffer_get(m_resource) : nullptr;
}

void ClientBuffer::unlock()
{
    if (--m_locks > 0)
        return;
    if (m_resource)
        wl_buffer_send_release(m_resource);
    else
        delete this; // client already destroyed it; we were the last user
}

void ClientBuffer::handleResourceDestroy(wl_listener* listener, void*)
{
    auto* self = reinterpret_cast<Listener*>(listener)->owner;
    wl_list_remove(&listener->link);
    self->m_resource = nullptr;
    if (self->m_locks == 0)
        delete self;
}

} // namespace dragon
