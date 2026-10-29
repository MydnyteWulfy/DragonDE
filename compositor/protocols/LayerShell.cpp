#include "protocols/LayerShell.h"

#include "Server.h"
#include "core/Log.h"
#include "protocols/XdgShell.h"
#include "wayland/Global.h"

#include <algorithm>
#include <bit>

namespace dragon {

namespace {

constexpr std::uint32_t kLayerShellVersion = 5;
constexpr const char* kLayerRole = "zwlr_layer_surface_v1";
constexpr std::uint32_t kAllAnchors = LayerSurface::kTop | LayerSurface::kBottom | LayerSurface::kLeft |
                                      LayerSurface::kRight;

class LayerShellResource final : public proto::ZwlrLayerShellV1 {
public:
    LayerShellResource(wl_client* client, std::uint32_t version, std::uint32_t id, LayerShell& shell)
        : ZwlrLayerShellV1(client, version, id), m_shell(shell) {}

protected:
    void handleGetLayerSurface(std::uint32_t id, wl_resource* surfaceResource, wl_resource*,
                               proto::ZwlrLayerShellV1Layer layer, std::string_view nameSpace) override
    {
        // The output is ignored for now: one output, everything goes there.
        Surface* surface = Surface::from(surfaceResource);
        if (!surface)
            return;
        if (std::uint32_t(layer) > std::uint32_t(proto::ZwlrLayerShellV1Layer::Overlay)) {
            postError(Error::InvalidLayer, "unknown layer {}", std::uint32_t(layer));
            return;
        }
        if (surface->role() || !surface->setRoleName(kLayerRole)) {
            postError(Error::Role, "wl_surface already has a role");
            return;
        }
        if (surface->hasContent()) {
            postError(Error::AlreadyConstructed, "wl_surface already has a buffer");
            return;
        }
        if (auto* s = makeResource<LayerSurface>(client(), version(), id, surface, layer, std::string(nameSpace)))
            m_shell.surfaceCreated.emit(s);
    }

private:
    LayerShell& m_shell;
};

} // namespace

// ------------------------------------------------------------ LayerSurface --

LayerSurface::LayerSurface(wl_client* client, std::uint32_t version, std::uint32_t id, Surface* surface, Layer layer,
                           std::string nameSpace)
    : ZwlrLayerSurfaceV1(client, version, id), m_surface(surface), m_nameSpace(std::move(nameSpace))
{
    m_pending.layer = m_current.layer = layer;
    surface->attachRoleObject(this);
    m_surfaceDying = surface->aboutToBeDestroyed.connect([this] {
        if (m_mapped)
            unmap();
        m_surface = nullptr;
    });
}

LayerSurface::~LayerSurface()
{
    if (m_mapped)
        unmap();
    if (m_surface)
        m_surface->detachRoleObject(this);
}

std::uint32_t LayerSurface::exclusiveEdge() const
{
    const State& s = m_current;
    if (s.exclusiveEdge)
        return s.exclusiveEdge;
    // One edge, or one edge and both its neighbours (a bar along the top).
    if (std::popcount(s.anchor) == 1)
        return s.anchor;
    for (std::uint32_t edge : {kTop, kBottom, kLeft, kRight}) {
        const std::uint32_t across = (edge == kTop || edge == kBottom) ? kLeft | kRight : kTop | kBottom;
        if (s.anchor == (edge | across))
            return edge;
    }
    return 0;
}

void LayerSurface::configure(Size size)
{
    if (m_closed || (m_lastSent && *m_lastSent == size))
        return;
    m_lastSent = size;
    const std::uint32_t serial = wl_display_next_serial(wl_client_get_display(client()));
    m_sentSerials.push_back(serial);
    sendConfigure(serial, std::uint32_t(std::max(size.width, 0)), std::uint32_t(std::max(size.height, 0)));
}

void LayerSurface::close()
{
    if (m_closed)
        return;
    m_closed = true;
    sendClosed();
}

bool LayerSurface::precommit(const SurfaceState& pending)
{
    if ((pending.committed & SurfaceState::BufferField) && pending.buffer && !m_configured) {
        postError(Error::InvalidSurfaceState, "buffer attached before the first configure was acknowledged");
        return false;
    }
    const State& s = m_pending;
    if ((s.size.width == 0 && (s.anchor & (kLeft | kRight)) != (kLeft | kRight)) ||
        (s.size.height == 0 && (s.anchor & (kTop | kBottom)) != (kTop | kBottom))) {
        postError(Error::InvalidSize, "a size of 0 needs anchors on both opposite edges");
        return false;
    }
    if (s.exclusiveEdge && !(s.anchor & s.exclusiveEdge)) {
        postError(Error::InvalidExclusiveEdge, "the exclusive edge must be one the surface is anchored to");
        return false;
    }
    return true;
}

void LayerSurface::commit()
{
    if (!m_surface)
        return;
    const bool changed = m_pending != m_current;
    m_current = m_pending;

    if (!m_initialCommitDone) {
        // The initial commit (no buffer): the desktop places it and configures.
        m_initialCommitDone = true;
        stateChanged.emit();
        return;
    }
    const bool hasBuffer = m_surface->hasContent();
    if (changed)
        stateChanged.emit();
    if (hasBuffer && !m_mapped) {
        m_mapped = true;
        mapped.emit();
    } else if (!hasBuffer && m_mapped) {
        unmap();
    }
}

void LayerSurface::unmap()
{
    m_mapped = false;
    // Back to the start: a fresh initial commit and configure before mapping again.
    m_initialCommitDone = false;
    m_configured = false;
    m_lastSent.reset();
    m_sentSerials.clear();
    unmapped.emit();
}

void LayerSurface::handleSetSize(std::uint32_t width, std::uint32_t height)
{
    if (width > std::uint32_t(INT32_MAX) || height > std::uint32_t(INT32_MAX)) {
        postError(Error::InvalidSize, "size {}x{} is too large", width, height);
        return;
    }
    m_pending.size = {std::int32_t(width), std::int32_t(height)};
}

void LayerSurface::handleSetAnchor(proto::ZwlrLayerSurfaceV1Anchor anchor)
{
    if (std::uint32_t(anchor) & ~kAllAnchors) {
        postError(Error::InvalidAnchor, "unknown anchor bits {:#x}", std::uint32_t(anchor));
        return;
    }
    m_pending.anchor = std::uint32_t(anchor);
}

void LayerSurface::handleSetExclusiveZone(std::int32_t zone)
{
    m_pending.exclusiveZone = std::max(zone, -1);
}

void LayerSurface::handleSetMargin(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left)
{
    m_pending.marginTop = top;
    m_pending.marginRight = right;
    m_pending.marginBottom = bottom;
    m_pending.marginLeft = left;
}

void LayerSurface::handleSetKeyboardInteractivity(Keyboard keyboard)
{
    const auto max = version() >= 4 ? Keyboard::OnDemand : Keyboard::Exclusive;
    if (std::uint32_t(keyboard) > std::uint32_t(max)) {
        postError(Error::InvalidKeyboardInteractivity, "unknown keyboard interactivity {}", std::uint32_t(keyboard));
        return;
    }
    m_pending.keyboard = keyboard;
}

void LayerSurface::handleGetPopup(wl_resource* popupResource)
{
    auto* popup = static_cast<XdgPopup*>(proto::XdgPopup::fromResource(popupResource));
    if (!popup || !m_surface)
        return;
    popup->setForeignParent(m_surface);
}

void LayerSurface::handleAckConfigure(std::uint32_t serial)
{
    auto it = std::ranges::find(m_sentSerials, serial);
    if (it == m_sentSerials.end()) {
        postError(Error::InvalidSurfaceState, "ack_configure({}) doesn't match a sent configure", serial);
        return;
    }
    m_sentSerials.erase(m_sentSerials.begin(), it + 1);
    m_configured = true;
}

void LayerSurface::handleSetLayer(Layer layer)
{
    if (std::uint32_t(layer) > std::uint32_t(Layer::Overlay)) {
        wl_resource_post_error(resource(), std::uint32_t(proto::ZwlrLayerShellV1Error::InvalidLayer), "unknown layer");
        return;
    }
    m_pending.layer = layer;
}

void LayerSurface::handleSetExclusiveEdge(proto::ZwlrLayerSurfaceV1Anchor edge)
{
    const auto e = std::uint32_t(edge);
    if (e != 0 && std::popcount(e) != 1) {
        postError(Error::InvalidExclusiveEdge, "the exclusive edge must be a single edge");
        return;
    }
    m_pending.exclusiveEdge = e;
}

// -------------------------------------------------------------- LayerShell --

LayerShell::LayerShell(Server& server)
{
    m_global = Global::create<proto::ZwlrLayerShellV1>(
        server.display(), kLayerShellVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<LayerShellResource>(client, version, id, *this);
        });
}

LayerShell::~LayerShell() = default;

} // namespace dragon
