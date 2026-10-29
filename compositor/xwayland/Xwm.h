#pragma once

#include "core/EventLoop.h"
#include "core/Signal.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

struct xcb_connection_t;

namespace dragon {

class Desktop;
class Server;
class XwaylandShell;
class XwaylandWindow;

// The X11 window manager for Xwayland (over its -wm socket). X windows become
// desktop windows like any other: it tracks their lifecycle and properties,
// pairs each with its wl_surface (WL_SURFACE_SERIAL / xwayland_shell_v1),
// positions them, routes X focus, and turns EWMH requests (fullscreen,
// maximize, activate, move/resize) into desktop actions. Override-redirect
// windows (menus, tooltips) place themselves and stay unmanaged.
class Xwm {
public:
    Xwm(Server& server, EventLoop& loop, Desktop& desktop, XwaylandShell& shell, int wmFd);
    ~Xwm();
    Xwm(const Xwm&) = delete;
    Xwm& operator=(const Xwm&) = delete;

    bool ok() const { return m_ok; }

    enum Atom : int;
    struct Atoms;

private:
    friend class XwaylandWindow;

    bool setupRoot();
    void dispatch();
    void handleEvent(const void* event, std::uint8_t type);

    void onCreate(const void* e);
    void onDestroy(const void* e);
    void onMapRequest(const void* e);
    void onMap(const void* e);
    void onUnmap(const void* e);
    void onConfigureRequest(const void* e);
    void onConfigure(const void* e);
    void onProperty(const void* e);
    void onClientMessage(const void* e);

    XwaylandWindow* find(std::uint32_t window) const;
    void associate(XwaylandWindow& window, std::uint64_t serial);
    void readProperties(XwaylandWindow& window);
    void readProperty(XwaylandWindow& window, std::uint32_t atom);
    void focusX(XwaylandWindow* window);

    Server& m_server;
    EventLoop& m_loop;
    Desktop& m_desktop;
    XwaylandShell& m_shell;
    xcb_connection_t* m_conn = nullptr;
    bool m_ok = false;
    std::uint32_t m_root = 0;
    std::uint32_t m_wmWindow = 0;
    std::unique_ptr<Atoms> m_atoms;

    std::unordered_map<std::uint32_t, std::unique_ptr<XwaylandWindow>> m_windows;
    XwaylandWindow* m_focused = nullptr;

    EventSource m_source;
    Connection m_flush;
    Connection m_associated;
    Connection m_focusChanged;
};

} // namespace dragon
