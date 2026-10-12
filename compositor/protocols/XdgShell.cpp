#include "protocols/XdgShell.h"

#include "Server.h"
#include "core/EventLoop.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <wayland-server-core.h>

namespace dragon {

namespace {

constexpr const char* kToplevelRole = "xdg_toplevel";
constexpr const char* kPopupRole = "xdg_popup";

// Anchor and gravity share one numbering: none, top, bottom, left, right,
// top_left, bottom_left, top_right, bottom_right.
bool hasLeft(std::uint32_t v) { return v == 3 || v == 5 || v == 6; }
bool hasRight(std::uint32_t v) { return v == 4 || v == 7 || v == 8; }
bool hasTop(std::uint32_t v) { return v == 1 || v == 5 || v == 7; }
bool hasBottom(std::uint32_t v) { return v == 2 || v == 6 || v == 8; }

} // namespace

// ---------------------------------------------------------- PositionerState --

Rect PositionerState::place() const
{
    const auto a = std::uint32_t(anchor);
    const auto g = std::uint32_t(gravity);

    Point p{anchorRect.x + anchorRect.width / 2, anchorRect.y + anchorRect.height / 2};
    if (hasLeft(a))
        p.x = anchorRect.x;
    else if (hasRight(a))
        p.x = anchorRect.x + anchorRect.width;
    if (hasTop(a))
        p.y = anchorRect.y;
    else if (hasBottom(a))
        p.y = anchorRect.y + anchorRect.height;

    // Gravity: which way the popup extends from the anchor point.
    if (hasLeft(g))
        p.x -= size.width;
    else if (!hasRight(g))
        p.x -= size.width / 2;
    if (hasTop(g))
        p.y -= size.height;
    else if (!hasBottom(g))
        p.y -= size.height / 2;

    return {p.x + offset.x, p.y + offset.y, size.width, size.height};
}

// ------------------------------------------------------------ XdgPositioner --

const PositionerState* XdgPositioner::from(wl_resource* resource)
{
    auto* p = static_cast<XdgPositioner*>(proto::XdgPositioner::fromResource(resource));
    return p ? &p->m_state : nullptr;
}

void XdgPositioner::handleSetSize(std::int32_t width, std::int32_t height)
{
    if (width <= 0 || height <= 0) {
        postError(proto::XdgPositionerError::InvalidInput, "set_size({}, {}): must be positive", width, height);
        return;
    }
    m_state.size = {width, height};
    m_state.hasSize = true;
}

void XdgPositioner::handleSetAnchorRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    if (width < 0 || height < 0) {
        postError(proto::XdgPositionerError::InvalidInput, "set_anchor_rect: negative size {}x{}", width, height);
        return;
    }
    m_state.anchorRect = {x, y, width, height};
    m_state.hasAnchorRect = true;
}

void XdgPositioner::handleSetAnchor(proto::XdgPositionerAnchor anchor)
{
    if (std::uint32_t(anchor) > 8) {
        postError(proto::XdgPositionerError::InvalidInput, "invalid anchor {}", std::uint32_t(anchor));
        return;
    }
    m_state.anchor = anchor;
}

void XdgPositioner::handleSetGravity(proto::XdgPositionerGravity gravity)
{
    if (std::uint32_t(gravity) > 8) {
        postError(proto::XdgPositionerError::InvalidInput, "invalid gravity {}", std::uint32_t(gravity));
        return;
    }
    m_state.gravity = gravity;
}

void XdgPositioner::handleSetConstraintAdjustment(proto::XdgPositionerConstraintAdjustment adjustment)
{
    m_state.constraintAdjustment = adjustment;
}

void XdgPositioner::handleSetOffset(std::int32_t x, std::int32_t y)
{
    m_state.offset = {x, y};
}

void XdgPositioner::handleSetReactive()
{
    m_state.reactive = true;
}

// ---------------------------------------------------------------- XdgWmBase --

void XdgWmBase::handleDestroy()
{
    if (m_surfaces > 0)
        postError(proto::XdgWmBaseError::DefunctSurfaces, "xdg_wm_base destroyed with {} xdg_surface(s) alive",
                  m_surfaces);
}

void XdgWmBase::handleCreatePositioner(std::uint32_t id)
{
    makeResource<XdgPositioner>(client(), version(), id);
}

void XdgWmBase::handleGetXdgSurface(std::uint32_t id, wl_resource* surface)
{
    Surface* s = Surface::from(surface);
    if (!s)
        return;
    if (s->role()) {
        postError(proto::XdgWmBaseError::Role, "wl_surface already has an active role object");
        return;
    }
    if (s->roleName() && std::strcmp(s->roleName(), kToplevelRole) != 0 &&
        std::strcmp(s->roleName(), kPopupRole) != 0) {
        postError(proto::XdgWmBaseError::Role, "wl_surface already has the role {}", s->roleName());
        return;
    }
    if (s->buffer()) {
        postError(proto::XdgWmBaseError::InvalidSurfaceState, "wl_surface already has a buffer committed");
        return;
    }

    auto* xdg = makeResource<XdgSurface>(client(), version(), id, m_server, s);
    if (!xdg)
        return;
    ++m_surfaces;
    std::erase_if(m_surfaceConnections, [](const Connection& c) { return !c.connected(); });
    m_surfaceConnections.push_back(xdg->destroyed.connect([this] { --m_surfaces; }));
}

// --------------------------------------------------------------- XdgSurface --

XdgSurface::XdgSurface(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server, Surface* surface)
    : proto::XdgSurface(client, version, id), m_server(server), m_surface(surface)
{
    surface->attachRoleObject(this);
    m_surfaceDying = surface->aboutToBeDestroyed.connect([this] {
        // A protocol violation (the role object must go first), but survivable.
        if (m_mapped)
            unmap();
        m_surface = nullptr;
    });
}

XdgSurface::~XdgSurface()
{
    if (m_mapped)
        unmap();
    // Destroyed before its role object (a client error, reported in
    // handleDestroy): the role object becomes inert.
    if (m_toplevel)
        m_toplevel->m_xdg = nullptr;
    if (m_popup)
        m_popup->m_xdg = nullptr;
    if (m_surface)
        m_surface->detachRoleObject(this);
}

Rect XdgSurface::geometry() const
{
    if (m_geometry)
        return *m_geometry;
    const Size s = m_surface ? m_surface->size() : Size{};
    return {0, 0, s.width, s.height};
}

void XdgSurface::scheduleConfigure()
{
    if (m_configureScheduled || !m_role)
        return;
    m_configureScheduled = true;
    std::weak_ptr<char> alive = m_alive;
    m_server.loop().addIdle([this, alive] {
        if (alive.expired())
            return;
        m_configureScheduled = false;
        sendConfigureNow();
    });
}

void XdgSurface::sendConfigureNow()
{
    if (!m_role)
        return;
    const std::uint32_t serial = m_server.nextSerial();
    m_role->sendRoleConfigure(serial);
    sendConfigure(serial);
    m_sentSerials.push_back(serial);
}

bool XdgSurface::precommit(const SurfaceState& pending)
{
    if (!m_role) {
        if (m_roleAssigned)
            return true; // role object destroyed: surface is unmapped, commits are harmless
        postError(proto::XdgSurfaceError::NotConstructed, "xdg_surface committed before get_toplevel/get_popup");
        return false;
    }
    if ((pending.committed & SurfaceState::BufferField) && pending.buffer && !m_configured) {
        postError(proto::XdgSurfaceError::UnconfiguredBuffer,
                  "buffer attached before the first configure was acknowledged");
        return false;
    }
    return true;
}

void XdgSurface::commit()
{
    if (m_pendingGeometry)
        m_geometry = std::exchange(m_pendingGeometry, std::nullopt);
    if (!m_role || !m_surface)
        return;

    m_role->committed();

    if (!m_initialCommitDone) {
        // The initial commit (no buffer) asks for the first configure.
        m_initialCommitDone = true;
        scheduleConfigure();
        return;
    }

    const bool hasBuffer = m_surface->buffer() != nullptr;
    if (hasBuffer && !m_mapped) {
        m_mapped = true;
        mapped.emit();
    } else if (!hasBuffer && m_mapped) {
        unmap();
    }
}

void XdgSurface::unmap()
{
    // Back to the initial state: the client must do a fresh initial commit.
    m_mapped = false;
    m_configured = false;
    m_initialCommitDone = false;
    m_sentSerials.clear();
    unmapped.emit();
}

void XdgSurface::roleDestroyed()
{
    if (m_mapped)
        unmap();
    m_role = nullptr;
    m_toplevel = nullptr;
    m_popup = nullptr;
}

void XdgSurface::handleDestroy()
{
    if (m_role)
        postError(proto::XdgSurfaceError::DefunctRoleObject, "xdg_surface destroyed before its role object");
}

void XdgSurface::handleGetToplevel(std::uint32_t id)
{
    if (!m_surface)
        return;
    if (m_roleAssigned) {
        postError(proto::XdgSurfaceError::AlreadyConstructed, "xdg_surface already has a role");
        return;
    }
    if (!m_surface->setRoleName(kToplevelRole)) {
        postError(proto::XdgSurfaceError::AlreadyConstructed, "wl_surface already had the role {}",
                  m_surface->roleName());
        return;
    }
    auto* toplevel = makeResource<XdgToplevel>(client(), version(), id, this);
    if (!toplevel)
        return;
    m_roleAssigned = true;
    m_role = toplevel;
    m_toplevel = toplevel;
    m_server.toplevelCreated.emit(toplevel);
}

void XdgSurface::handleGetPopup(std::uint32_t id, wl_resource* parent, wl_resource* positioner)
{
    if (!m_surface)
        return;
    const PositionerState* pos = XdgPositioner::from(positioner);
    if (!pos || !pos->complete()) {
        if (auto* p = proto::XdgPositioner::fromResource(positioner))
            p->postError(proto::XdgPositionerError::InvalidInput, "positioner needs set_size and set_anchor_rect");
        return;
    }
    if (m_roleAssigned) {
        postError(proto::XdgSurfaceError::AlreadyConstructed, "xdg_surface already has a role");
        return;
    }
    if (!m_surface->setRoleName(kPopupRole)) {
        postError(proto::XdgSurfaceError::AlreadyConstructed, "wl_surface already had the role {}",
                  m_surface->roleName());
        return;
    }
    auto* popup = makeResource<XdgPopup>(client(), version(), id, this, parent ? from(parent) : nullptr, *pos);
    if (!popup)
        return;
    m_roleAssigned = true;
    m_role = popup;
    m_popup = popup;
    m_server.popupCreated.emit(popup);
}

void XdgSurface::handleSetWindowGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    if (width <= 0 || height <= 0) {
        postError(proto::XdgSurfaceError::InvalidSize, "window geometry {}x{} must be positive", width, height);
        return;
    }
    m_pendingGeometry = Rect{x, y, width, height};
}

void XdgSurface::handleAckConfigure(std::uint32_t serial)
{
    if (!m_role) {
        if (!m_roleAssigned)
            postError(proto::XdgSurfaceError::NotConstructed, "ack_configure before get_toplevel/get_popup");
        return;
    }
    auto it = std::ranges::find(m_sentSerials, serial);
    if (it == m_sentSerials.end()) {
        postError(proto::XdgSurfaceError::InvalidSerial, "ack_configure({}) doesn't match a sent configure", serial);
        return;
    }
    m_sentSerials.erase(m_sentSerials.begin(), it + 1); // acking a configure skips older ones
    m_configured = true;
    m_role->acked(serial);
}

// -------------------------------------------------------------- XdgToplevel --

XdgToplevel::XdgToplevel(wl_client* client, std::uint32_t version, std::uint32_t id, XdgSurface* xdg)
    : proto::XdgToplevel(client, version, id), m_xdg(xdg)
{
}

XdgToplevel::~XdgToplevel()
{
    if (m_xdg)
        m_xdg->roleDestroyed();
}

void XdgToplevel::update(bool Configure::*field, bool on)
{
    if (m_pending.*field == on)
        return;
    m_pending.*field = on;
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::setSize(Size size)
{
    if (m_pending.size == size)
        return;
    m_pending.size = size;
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::setActivated(bool on) { update(&Configure::activated, on); }
void XdgToplevel::setMaximized(bool on) { update(&Configure::maximized, on); }
void XdgToplevel::setFullscreen(bool on) { update(&Configure::fullscreen, on); }
void XdgToplevel::setResizing(bool on) { update(&Configure::resizing, on); }

void XdgToplevel::sendRoleConfigure(std::uint32_t serial)
{
    if (!m_sentCapabilities && version() >= 5) {
        // Nothing yet: clients hide maximize/fullscreen/minimize/menu buttons.
        wl_array caps;
        wl_array_init(&caps);
        sendWmCapabilities(&caps);
        wl_array_release(&caps);
        m_sentCapabilities = true;
    }

    wl_array states;
    wl_array_init(&states);
    auto push = [&states](proto::XdgToplevelState s) {
        if (auto* p = static_cast<std::uint32_t*>(wl_array_add(&states, sizeof(std::uint32_t))))
            *p = std::uint32_t(s);
    };
    if (m_pending.maximized)
        push(proto::XdgToplevelState::Maximized);
    if (m_pending.fullscreen)
        push(proto::XdgToplevelState::Fullscreen);
    if (m_pending.resizing)
        push(proto::XdgToplevelState::Resizing);
    if (m_pending.activated)
        push(proto::XdgToplevelState::Activated);

    sendConfigure(m_pending.size.width, m_pending.size.height, &states);
    wl_array_release(&states);
    m_inflight.emplace_back(serial, m_pending);
}

void XdgToplevel::acked(std::uint32_t serial)
{
    auto it = std::ranges::find_if(m_inflight, [serial](const auto& p) { return p.first == serial; });
    if (it == m_inflight.end())
        return;
    m_current = it->second;
    m_inflight.erase(m_inflight.begin(), it + 1);
}

void XdgToplevel::committed()
{
    if (m_pendingMaxSize.width > 0 && m_pendingMinSize.width > m_pendingMaxSize.width) {
        postError(proto::XdgToplevelError::InvalidSize, "min width {} > max width {}", m_pendingMinSize.width,
                  m_pendingMaxSize.width);
        return;
    }
    if (m_pendingMaxSize.height > 0 && m_pendingMinSize.height > m_pendingMaxSize.height) {
        postError(proto::XdgToplevelError::InvalidSize, "min height {} > max height {}", m_pendingMinSize.height,
                  m_pendingMaxSize.height);
        return;
    }
    m_minSize = m_pendingMinSize;
    m_maxSize = m_pendingMaxSize;
}

void XdgToplevel::handleSetParent(wl_resource* parent)
{
    XdgToplevel* p = parent ? from(parent) : nullptr;
    for (auto* a = p; a; a = a->m_parent) {
        if (a == this) {
            postError(proto::XdgToplevelError::InvalidParent, "set_parent would create a loop");
            return;
        }
    }
    m_parent = p;
    m_parentGone = p ? p->destroyed.connect([this] { m_parent = nullptr; }) : Connection{};
}

void XdgToplevel::handleSetTitle(std::string_view title)
{
    m_title = title;
    titleChanged.emit();
}

void XdgToplevel::handleSetAppId(std::string_view appId)
{
    m_appId = appId;
    appIdChanged.emit();
}

void XdgToplevel::handleMove(wl_resource*, std::uint32_t serial)
{
    moveRequested.emit(serial);
}

void XdgToplevel::handleResize(wl_resource*, std::uint32_t serial, proto::XdgToplevelResizeEdge edges)
{
    switch (std::uint32_t(edges)) {
    case 0: case 1: case 2: case 4: case 5: case 6: case 8: case 9: case 10: break;
    default:
        postError(proto::XdgToplevelError::InvalidResizeEdge, "invalid resize edge {}", std::uint32_t(edges));
        return;
    }
    resizeRequested.emit(serial, edges);
}

void XdgToplevel::handleSetMaxSize(std::int32_t width, std::int32_t height)
{
    if (width < 0 || height < 0) {
        postError(proto::XdgToplevelError::InvalidSize, "max size {}x{} is negative", width, height);
        return;
    }
    m_pendingMaxSize = {width, height};
}

void XdgToplevel::handleSetMinSize(std::int32_t width, std::int32_t height)
{
    if (width < 0 || height < 0) {
        postError(proto::XdgToplevelError::InvalidSize, "min size {}x{} is negative", width, height);
        return;
    }
    m_pendingMinSize = {width, height};
}

// The protocol requires a configure in reply even when the request isn't honored.
void XdgToplevel::handleSetMaximized()
{
    maximizeRequested.emit(true);
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::handleUnsetMaximized()
{
    maximizeRequested.emit(false);
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::handleSetFullscreen(wl_resource*)
{
    fullscreenRequested.emit(true);
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::handleUnsetFullscreen()
{
    fullscreenRequested.emit(false);
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgToplevel::handleSetMinimized()
{
    minimizeRequested.emit();
}

// ----------------------------------------------------------------- XdgPopup --

XdgPopup::XdgPopup(wl_client* client, std::uint32_t version, std::uint32_t id, XdgSurface* xdg, XdgSurface* parent,
                   const PositionerState& positioner)
    : proto::XdgPopup(client, version, id), m_xdg(xdg), m_parent(parent), m_geometry(positioner.place())
{
    if (parent) {
        m_parentDying = parent->destroyed.connect([this] {
            m_parent = nullptr;
            dismiss();
        });
    }
}

XdgPopup::~XdgPopup()
{
    if (m_xdg)
        m_xdg->roleDestroyed();
}

void XdgPopup::dismiss()
{
    if (m_dismissed)
        return;
    m_dismissed = true;
    sendPopupDone();
}

void XdgPopup::handleReposition(wl_resource* positioner, std::uint32_t token)
{
    const PositionerState* pos = XdgPositioner::from(positioner);
    if (!pos || !pos->complete()) {
        if (auto* p = proto::XdgPositioner::fromResource(positioner))
            p->postError(proto::XdgPositionerError::InvalidInput, "positioner needs set_size and set_anchor_rect");
        return;
    }
    m_geometry = pos->place();
    m_repositionToken = token;
    if (m_xdg)
        m_xdg->scheduleConfigure();
}

void XdgPopup::sendRoleConfigure(std::uint32_t)
{
    if (m_repositionToken)
        sendRepositioned(*std::exchange(m_repositionToken, std::nullopt));
    sendConfigure(m_geometry.x, m_geometry.y, m_geometry.width, m_geometry.height);
}

} // namespace dragon
