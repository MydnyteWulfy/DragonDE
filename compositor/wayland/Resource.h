#pragma once

#include "core/Signal.h"

#include <cstdint>
#include <format>
#include <string>
#include <utility>

struct wl_client;
struct wl_interface;
struct wl_resource;

namespace dragon {

// Base of every generated protocol class (dragon::proto::*).
//
// Lifetime: a Resource is heap-allocated and owned by its wl_resource. When
// the client destroys the object (destructor request, disconnect) or the
// compositor calls destroy(), libwayland runs our destroy handler, which
// deletes the C++ object. Deleting a Resource directly also destroys its
// wl_resource. Either way, `destroyed` fires exactly once.
class Resource {
public:
    virtual ~Resource();
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    // False only if wl_resource_create failed (out of memory). Use makeResource().
    bool isValid() const { return m_resource != nullptr; }

    wl_resource* resource() const { return m_resource; }
    wl_client* client() const { return m_client; }
    std::uint32_t version() const { return m_version; }
    std::uint32_t id() const;

    // Destroys the wl_resource, which deletes this object. Don't touch `this` afterwards.
    // Only valid when the protocol says the server destroys the object (e.g.
    // wl_callback after `done`). Otherwise the client will still send requests
    // for it and get disconnected with "invalid object"; make it inert instead.
    void destroy();

    // Sends a protocol error; the client will be disconnected.
    template <typename... Args>
    void postError(std::uint32_t code, std::format_string<Args...> fmt, Args&&... args)
    {
        postErrorString(code, std::format(fmt, std::forward<Args>(args)...));
    }

    // Emitted from ~Resource. Derived parts are already gone, so listeners may
    // only use the pointer for identity (e.g. to remove it from a list).
    Signal<> destroyed;

protected:
    Resource(wl_client* client, const wl_interface* interface, std::uint32_t version,
             std::uint32_t id, const void* implementation);

    // Generated fromResource() helpers use this to check the interface/implementation.
    static Resource* fromResourceChecked(wl_resource* resource, const wl_interface* interface,
                                         const void* implementation);

private:
    static void handleWlDestroy(wl_resource* resource);
    void postErrorString(std::uint32_t code, const std::string& message);

    wl_resource* m_resource = nullptr;
    wl_client* m_client = nullptr;
    std::uint32_t m_version = 0;
};

// Creates a resource-backed object, returning nullptr (and freeing it) if
// wl_resource_create failed. The client has already been sent no_memory then.
template <typename T, typename... Args>
T* makeResource(Args&&... args)
{
    auto* obj = new T(std::forward<Args>(args)...);
    if (!obj->isValid()) {
        delete obj;
        return nullptr;
    }
    return obj;
}

} // namespace dragon
