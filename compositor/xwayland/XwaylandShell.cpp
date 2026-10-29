#include "xwayland/XwaylandShell.h"

#include "Server.h"
#include "dragon-protocols/xwayland-shell-v1.h"
#include "protocols/Compositor.h"

namespace dragon {

namespace {

constexpr const char* kRole = "xwayland_surface_v1";

// The role object: holds the serial until the surface commits it.
class XwaylandSurface final : public proto::XwaylandSurfaceV1, public SurfaceRole {
public:
    XwaylandSurface(wl_client* client, std::uint32_t version, std::uint32_t id, XwaylandShell& shell, Surface* surface)
        : XwaylandSurfaceV1(client, version, id), m_shell(shell), m_surface(surface)
    {
        surface->attachRoleObject(this);
        m_surfaceGone = surface->aboutToBeDestroyed.connect([this] {
            if (m_serial)
                m_shell.removeSerial(m_serial);
            m_surface = nullptr;
        });
    }

    ~XwaylandSurface() override
    {
        if (m_surface)
            m_surface->detachRoleObject(this);
        // The association lives as long as the surface: X windows outlive this object.
    }

    void commit() override
    {
        if (!m_pending || !m_surface)
            return;
        m_serial = m_pending;
        m_pending = 0;
        m_shell.addSerial(m_serial, m_surface);
    }

protected:
    void handleSetSerial(std::uint32_t lo, std::uint32_t hi) override
    {
        const std::uint64_t serial = (std::uint64_t(hi) << 32) | lo;
        if (m_serial || m_pending) {
            postError(proto::XwaylandSurfaceV1Error::AlreadyAssociated, "serial already set");
            return;
        }
        if (serial == 0) {
            postError(proto::XwaylandSurfaceV1Error::InvalidSerial, "serial 0 is invalid");
            return;
        }
        m_pending = serial; // double-buffered: applies on the next commit
    }

private:
    XwaylandShell& m_shell;
    Surface* m_surface;
    std::uint64_t m_pending = 0;
    std::uint64_t m_serial = 0;
    Connection m_surfaceGone;
};

class ShellResource final : public proto::XwaylandShellV1 {
public:
    ShellResource(wl_client* client, std::uint32_t version, std::uint32_t id, XwaylandShell& shell)
        : XwaylandShellV1(client, version, id), m_shell(shell) {}

protected:
    void handleGetXwaylandSurface(std::uint32_t id, wl_resource* surface) override
    {
        Surface* s = Surface::from(surface);
        if (!s || s->role() || !s->setRoleName(kRole)) {
            postError(proto::XwaylandShellV1Error::Role, "surface already has a role");
            return;
        }
        makeResource<XwaylandSurface>(client(), version(), id, m_shell, s);
    }

private:
    XwaylandShell& m_shell;
};

} // namespace

XwaylandShell::XwaylandShell(Server& server)
{
    m_global = Global::create<proto::XwaylandShellV1>(
        server.display(), 1, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<ShellResource>(client, version, id, *this);
        });
    server.restrictGlobal(m_global->handle(), [this](const wl_client* client) { return client && client == m_xwayland; });
}

XwaylandShell::~XwaylandShell() = default;

Surface* XwaylandShell::surfaceForSerial(std::uint64_t serial) const
{
    auto it = m_surfaces.find(serial);
    return it == m_surfaces.end() ? nullptr : it->second;
}

void XwaylandShell::addSerial(std::uint64_t serial, Surface* surface)
{
    m_surfaces[serial] = surface;
    associated.emit(surface, serial);
}

void XwaylandShell::removeSerial(std::uint64_t serial)
{
    m_surfaces.erase(serial);
}

} // namespace dragon
