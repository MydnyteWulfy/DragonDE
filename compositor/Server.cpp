#include "Server.h"

#include "backend/Backend.h"
#include "core/EventLoop.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "protocols/CursorShape.h"
#include "protocols/DataDevice.h"
#include "protocols/LayerShell.h"
#include "protocols/LinuxDmabuf.h"
#include "protocols/Output.h"
#include "protocols/Seat.h"
#include "protocols/XdgActivation.h"
#include "protocols/XdgDecoration.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <drm_fourcc.h>
#include <wayland-server-core.h>

namespace dragon {

namespace {
constexpr std::uint32_t kCompositorVersion = 6;
constexpr std::uint32_t kSubcompositorVersion = 1;
constexpr std::uint32_t kXdgWmBaseVersion = 6;
constexpr std::uint32_t kDataDeviceManagerVersion = 3;
} // namespace

Server::Server(wl_display* display, EventLoop& loop) : m_display(display), m_loop(loop)
{
    wl_display_set_global_filter(
        display,
        [](const wl_client* client, const wl_global* global, void* data) {
            auto* self = static_cast<Server*>(data);
            for (const auto& [g, allowed] : self->m_globalFilters) {
                if (g == global)
                    return allowed(client);
            }
            return true;
        },
        this);

    m_compositor = Global::create<proto::WlCompositor>(
        display, kCompositorVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<CompositorResource>(client, version, id, *this);
        });
    m_subcompositor = Global::create<proto::WlSubcompositor>(
        display, kSubcompositorVersion, [](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<SubcompositorResource>(client, version, id);
        });
    m_xdgWmBase = Global::create<proto::XdgWmBase>(
        display, kXdgWmBaseVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<XdgWmBase>(client, version, id, *this);
        });

    m_xdgDecorationManager = createXdgDecorationGlobal(display);
    m_xdgOutputManager = createXdgOutputManager(*this);

    m_seat = std::make_unique<Seat>(*this);
    m_dataDeviceManager = Global::create<proto::WlDataDeviceManager>(
        display, kDataDeviceManagerVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<DataDeviceManager>(client, version, id, *m_seat);
        });
    m_cursorShapeManager = std::make_unique<CursorShapeManager>(*this, *m_seat);
    m_activation = std::make_unique<XdgActivation>(*this);
    m_layerShell = std::make_unique<LayerShell>(*this);
    m_primarySelectionManager = Global::create<proto::ZwpPrimarySelectionDeviceManagerV1>(
        display, 1, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<PrimaryDeviceManager>(client, version, id, *m_seat);
        });

    // wl_shm comes from libwayland: it already guards against clients
    // shrinking their pools (SIGBUS) while we read them. ARGB/XRGB8888 are
    // implicit; add the byte-swapped variants many toolkits prefer.
    if (wl_display_init_shm(display) != 0)
        log::error("server", "wl_display_init_shm failed");
    wl_display_add_shm_format(display, DRM_FORMAT_ABGR8888);
    wl_display_add_shm_format(display, DRM_FORMAT_XBGR8888);

    m_flush = loop.beforeSleep.connect([this] { wl_display_flush_clients(m_display); });
}

Server::~Server() = default;

std::uint32_t Server::nextSerial()
{
    return wl_display_next_serial(m_display);
}

void Server::restrictGlobal(const wl_global* global, std::function<bool(const wl_client*)> allowed)
{
    std::erase_if(m_globalFilters, [global](const auto& f) { return f.first == global; });
    m_globalFilters.emplace_back(global, std::move(allowed));
}

void Server::enableDmabuf(DmabufSupport support)
{
    m_dmabuf = std::make_unique<LinuxDmabuf>(*this, std::move(support));
}

void Server::addOutput(Output& output)
{
    m_outputs.push_back(std::make_unique<OutputGlobal>(*this, output));
}

void Server::removeOutput(Output& output)
{
    std::erase_if(m_outputs, [&output](const auto& g) { return &g->output() == &output; });
}

OutputGlobal* Server::outputGlobalFor(wl_resource* wlOutput) const
{
    auto it = std::ranges::find_if(m_outputs, [wlOutput](const auto& g) { return g->owns(wlOutput); });
    return it == m_outputs.end() ? nullptr : it->get();
}

OutputGlobal* Server::outputGlobal(const Output& output) const
{
    auto it = std::ranges::find_if(m_outputs, [&output](const auto& g) { return &g->output() == &output; });
    return it == m_outputs.end() ? nullptr : it->get();
}

} // namespace dragon
