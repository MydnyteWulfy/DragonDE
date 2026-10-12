#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "dragon-protocols/xdg-shell.h"
#include "protocols/Compositor.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dragon {

class Server;
class XdgPopup;
class XdgSurface;
class XdgToplevel;

struct PositionerState {
    Size size;
    Rect anchorRect;
    bool hasSize = false;
    bool hasAnchorRect = false;
    proto::XdgPositionerAnchor anchor = proto::XdgPositionerAnchor::None;
    proto::XdgPositionerGravity gravity = proto::XdgPositionerGravity::None;
    proto::XdgPositionerConstraintAdjustment constraintAdjustment{};
    Point offset;
    bool reactive = false;

    bool complete() const { return hasSize && hasAnchorRect; }
    // Popup rectangle relative to the parent's window geometry. Constraint
    // adjustment (keeping popups on screen) comes with the scene graph.
    Rect place() const;
};

class XdgPositioner final : public proto::XdgPositioner {
public:
    using proto::XdgPositioner::XdgPositioner;
    static const PositionerState* from(wl_resource* resource);

protected:
    void handleSetSize(std::int32_t width, std::int32_t height) override;
    void handleSetAnchorRect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void handleSetAnchor(proto::XdgPositionerAnchor anchor) override;
    void handleSetGravity(proto::XdgPositionerGravity gravity) override;
    void handleSetConstraintAdjustment(proto::XdgPositionerConstraintAdjustment adjustment) override;
    void handleSetOffset(std::int32_t x, std::int32_t y) override;
    void handleSetReactive() override;
    void handleSetParentSize(std::int32_t, std::int32_t) override {}
    void handleSetParentConfigure(std::uint32_t) override {}

private:
    PositionerState m_state;
};

// What xdg_toplevel and xdg_popup implement for their xdg_surface.
class XdgRole {
public:
    virtual ~XdgRole() = default;
    // Role-specific configure event, sent right before xdg_surface.configure.
    virtual void sendRoleConfigure(std::uint32_t serial) = 0;
    virtual void acked(std::uint32_t /*serial*/) {}
    virtual void committed() {}
};

class XdgWmBase final : public proto::XdgWmBase {
public:
    XdgWmBase(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server)
        : proto::XdgWmBase(client, version, id), m_server(server) {}

protected:
    void handleDestroy() override;
    void handleCreatePositioner(std::uint32_t id) override;
    void handleGetXdgSurface(std::uint32_t id, wl_resource* surface) override;
    void handlePong(std::uint32_t) override {}

private:
    Server& m_server;
    int m_surfaces = 0;
    std::vector<Connection> m_surfaceConnections;
};

// The xdg_surface: configure/ack bookkeeping and map state shared by toplevels
// and popups. It's the wl_surface's role object.
class XdgSurface final : public proto::XdgSurface, public SurfaceRole {
public:
    XdgSurface(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server, Surface* surface);
    ~XdgSurface() override;

    static XdgSurface* from(wl_resource* resource)
    {
        return static_cast<XdgSurface*>(proto::XdgSurface::fromResource(resource));
    }

    Server& server() const { return m_server; }
    Surface* surface() const { return m_surface; } // null once the wl_surface is gone
    XdgToplevel* toplevel() const { return m_toplevel; }
    XdgPopup* popup() const { return m_popup; }

    bool isConfigured() const { return m_configured; }
    bool isMapped() const { return m_mapped; }
    // The "window" part of the surface (excludes client-side shadows), in surface coordinates.
    Rect geometry() const;

    // Queues a configure for the end of this event loop iteration (coalesced).
    void scheduleConfigure();

    Signal<> mapped;
    Signal<> unmapped;

    // SurfaceRole
    bool precommit(const SurfaceState& pending) override;
    void commit() override;

protected:
    void handleDestroy() override;
    void handleGetToplevel(std::uint32_t id) override;
    void handleGetPopup(std::uint32_t id, wl_resource* parent, wl_resource* positioner) override;
    void handleSetWindowGeometry(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void handleAckConfigure(std::uint32_t serial) override;

private:
    friend class XdgToplevel;
    friend class XdgPopup;

    void sendConfigureNow();
    void unmap();
    void roleDestroyed();

    Server& m_server;
    Surface* m_surface;
    XdgRole* m_role = nullptr;
    XdgToplevel* m_toplevel = nullptr;
    XdgPopup* m_popup = nullptr;
    bool m_roleAssigned = false; // a role is permanent, even after its object is destroyed

    bool m_initialCommitDone = false;
    bool m_configured = false;
    bool m_mapped = false;
    std::vector<std::uint32_t> m_sentSerials;

    std::optional<Rect> m_pendingGeometry;
    std::optional<Rect> m_geometry;

    bool m_configureScheduled = false;
    std::shared_ptr<char> m_alive = std::make_shared<char>(); // guards queued idle callbacks
    Connection m_surfaceDying;
};

class XdgToplevel final : public proto::XdgToplevel, public XdgRole {
public:
    // State the compositor asks the client to adopt.
    struct Configure {
        Size size; // 0x0: client picks
        bool activated = false;
        bool maximized = false;
        bool fullscreen = false;
        bool resizing = false;
        friend bool operator==(const Configure&, const Configure&) = default;
    };

    XdgToplevel(wl_client* client, std::uint32_t version, std::uint32_t id, XdgSurface* xdg);
    ~XdgToplevel() override;

    static XdgToplevel* from(wl_resource* resource)
    {
        return static_cast<XdgToplevel*>(proto::XdgToplevel::fromResource(resource));
    }

    XdgSurface* xdgSurface() const { return m_xdg; } // null once inert
    const std::string& title() const { return m_title; }
    const std::string& appId() const { return m_appId; }
    XdgToplevel* parentToplevel() const { return m_parent; }
    Size minSize() const { return m_minSize; }
    Size maxSize() const { return m_maxSize; }

    // What we'll send next; changing it schedules a configure.
    const Configure& pendingConfigure() const { return m_pending; }
    void setSize(Size size);
    void setActivated(bool on);
    void setMaximized(bool on);
    void setFullscreen(bool on);
    void setResizing(bool on);
    // The last configure the client acked.
    const Configure& currentConfigure() const { return m_current; }

    // Asks the client to close (it may refuse or ask the user first).
    void requestClose() { sendClose(); }

    Signal<> titleChanged;
    Signal<> appIdChanged;
    Signal<std::uint32_t> moveRequested;                                   // serial
    Signal<std::uint32_t, proto::XdgToplevelResizeEdge> resizeRequested;  // serial, edges
    Signal<bool> maximizeRequested;                                       // true: maximize
    Signal<bool> fullscreenRequested;
    Signal<> minimizeRequested;

protected:
    void handleSetParent(wl_resource* parent) override;
    void handleSetTitle(std::string_view title) override;
    void handleSetAppId(std::string_view appId) override;
    void handleShowWindowMenu(wl_resource*, std::uint32_t, std::int32_t, std::int32_t) override {}
    void handleMove(wl_resource* seat, std::uint32_t serial) override;
    void handleResize(wl_resource* seat, std::uint32_t serial, proto::XdgToplevelResizeEdge edges) override;
    void handleSetMaxSize(std::int32_t width, std::int32_t height) override;
    void handleSetMinSize(std::int32_t width, std::int32_t height) override;
    void handleSetMaximized() override;
    void handleUnsetMaximized() override;
    void handleSetFullscreen(wl_resource* output) override;
    void handleUnsetFullscreen() override;
    void handleSetMinimized() override;

private:
    friend class XdgSurface;

    // XdgRole
    void sendRoleConfigure(std::uint32_t serial) override;
    void acked(std::uint32_t serial) override;
    void committed() override;

    void update(bool Configure::*field, bool on);

    XdgSurface* m_xdg;
    std::string m_title;
    std::string m_appId;
    XdgToplevel* m_parent = nullptr;
    Connection m_parentGone;
    Size m_pendingMinSize, m_pendingMaxSize, m_minSize, m_maxSize;

    Configure m_pending;
    Configure m_current;
    std::vector<std::pair<std::uint32_t, Configure>> m_inflight;
    bool m_sentCapabilities = false;
};

class XdgPopup final : public proto::XdgPopup, public XdgRole {
public:
    XdgPopup(wl_client* client, std::uint32_t version, std::uint32_t id, XdgSurface* xdg, XdgSurface* parent,
             const PositionerState& positioner);
    ~XdgPopup() override;

    XdgSurface* xdgSurface() const { return m_xdg; }
    XdgSurface* parent() const { return m_parent; }
    // Relative to the parent's window geometry.
    Rect geometry() const { return m_geometry; }

    // Tells the client the popup was dismissed (click outside, parent gone, ...).
    void dismiss();

protected:
    void handleGrab(wl_resource*, std::uint32_t) override {}
    void handleReposition(wl_resource* positioner, std::uint32_t token) override;

private:
    friend class XdgSurface;

    void sendRoleConfigure(std::uint32_t serial) override;

    XdgSurface* m_xdg;
    XdgSurface* m_parent;
    Rect m_geometry;
    std::optional<std::uint32_t> m_repositionToken;
    bool m_dismissed = false;
    Connection m_parentDying;
};

} // namespace dragon
