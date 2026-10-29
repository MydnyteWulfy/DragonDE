#pragma once

#include "core/Signal.h"
#include "wayland/Global.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

struct wl_client;

namespace dragon {

class Server;
class Surface;

// xwayland_shell_v1: Xwayland tags each wl_surface it creates with a 64-bit
// serial, and puts the same serial on the matching X window
// (WL_SURFACE_SERIAL). That's how the X window manager learns which surface
// shows which X window. Only Xwayland may bind it.
class XwaylandShell {
public:
    explicit XwaylandShell(Server& server);
    ~XwaylandShell();
    XwaylandShell(const XwaylandShell&) = delete;
    XwaylandShell& operator=(const XwaylandShell&) = delete;

    // The client allowed to see the global (Xwayland's connection).
    void setXwaylandClient(wl_client* client) { m_xwayland = client; }

    // The surface committed with `serial`, if any.
    Surface* surfaceForSerial(std::uint64_t serial) const;
    // A surface's serial took effect (on its commit).
    Signal<Surface*, std::uint64_t> associated;

    // Called by the role objects.
    void addSerial(std::uint64_t serial, Surface* surface);
    void removeSerial(std::uint64_t serial);

private:
    std::unique_ptr<Global> m_global;
    wl_client* m_xwayland = nullptr;
    std::unordered_map<std::uint64_t, Surface*> m_surfaces;
};

} // namespace dragon
