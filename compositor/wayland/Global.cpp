#include "wayland/Global.h"

#include "core/Log.h"

#include <algorithm>
#include <wayland-server-core.h>

namespace dragon {

Global::Global(wl_display* display, const wl_interface* interface, std::uint32_t version, BindFn bind)
    : m_bind(std::move(bind))
{
    m_global = wl_global_create(display, interface, int(version), this, handleBind);
    if (!m_global)
        log::error("wayland", "wl_global_create({} v{}) failed", interface->name, version);
}

Global::~Global()
{
    if (m_global)
        wl_global_destroy(m_global);
}

std::uint32_t Global::checkVersion(std::uint32_t requested, std::uint32_t max)
{
    if (requested > max) {
        log::warn("wayland", "global version {} exceeds generated max {}, clamping", requested, max);
        return max;
    }
    return std::max<std::uint32_t>(requested, 1);
}

void Global::handleBind(wl_client* client, void* data, std::uint32_t version, std::uint32_t id)
{
    auto* self = static_cast<Global*>(data);
    self->m_bind(client, version, id);
}

} // namespace dragon
