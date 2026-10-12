#include "wayland/Resource.h"

#include <wayland-server-core.h>

namespace dragon {

Resource::Resource(wl_client* client, const wl_interface* interface, std::uint32_t version,
                   std::uint32_t id, const void* implementation)
    : m_client(client), m_version(version)
{
    m_resource = wl_resource_create(client, interface, int(version), id);
    if (!m_resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(m_resource, implementation, this, handleWlDestroy);
}

Resource::~Resource()
{
    if (m_resource) {
        // Deleted from C++: detach first so handleWlDestroy doesn't delete us again.
        wl_resource_set_user_data(m_resource, nullptr);
        wl_resource_set_destructor(m_resource, nullptr);
        wl_resource_destroy(std::exchange(m_resource, nullptr));
    }
    destroyed.emit();
}

std::uint32_t Resource::id() const
{
    return m_resource ? wl_resource_get_id(m_resource) : 0;
}

void Resource::destroy()
{
    if (m_resource)
        wl_resource_destroy(m_resource); // -> handleWlDestroy -> delete this
}

Resource* Resource::fromResourceChecked(wl_resource* resource, const wl_interface* interface,
                                        const void* implementation)
{
    if (!resource || !wl_resource_instance_of(resource, interface, implementation))
        return nullptr;
    return static_cast<Resource*>(wl_resource_get_user_data(resource));
}

void Resource::handleWlDestroy(wl_resource* resource)
{
    auto* self = static_cast<Resource*>(wl_resource_get_user_data(resource));
    if (!self)
        return;
    self->m_resource = nullptr; // libwayland frees it after this handler returns
    delete self;
}

void Resource::postErrorString(std::uint32_t code, const std::string& message)
{
    if (m_resource)
        wl_resource_post_error(m_resource, code, "%s", message.c_str());
}

} // namespace dragon
