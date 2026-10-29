#pragma once

#include "core/EventLoop.h"
#include "core/Signal.h"
#include "core/UniqueFd.h"

#include <string>
#include <sys/types.h>
#include <wayland-server-core.h>

namespace dragon {

class Server;

// Runs Xwayland for X11 apps. We own the X display (:N): its lock file and
// listening sockets, so X apps can connect before Xwayland exists. It's
// started on the first connection (an unused X server costs nothing) and
// restarted on the next one if it dies.
class XwaylandServer {
public:
    XwaylandServer(Server& server, EventLoop& loop);
    ~XwaylandServer();
    XwaylandServer(const XwaylandServer&) = delete;
    XwaylandServer& operator=(const XwaylandServer&) = delete;

    // Claims a free display and starts listening. False if none could be set up.
    bool listen();
    // Starts Xwayland now instead of waiting for a client.
    void start();

    const std::string& displayName() const { return m_displayName; } // ":N"
    bool running() const { return m_client != nullptr; }
    wl_client* client() const { return m_client; }

    // Xwayland was launched; `client` is its Wayland connection (before it binds anything).
    Signal<wl_client*> started;
    // Xwayland is ready: connect the window manager to `wmFd`.
    Signal<int> ready;
    // Xwayland went away (crash or shutdown): drop everything X11.
    Signal<> stopped;

private:
    bool claimDisplay(int display);
    void releaseDisplay();
    void armLazyStart();
    void onDisplayReady();
    static void clientDestroyed(wl_listener* listener, void* data);
    void onStopped();

    Server& m_server;
    EventLoop& m_loop;

    int m_display = -1;
    std::string m_displayName;
    std::string m_lockPath;
    std::string m_socketPath;
    UniqueFd m_abstractSocket; // "@/tmp/.X11-unix/XN"
    UniqueFd m_socket;         // /tmp/.X11-unix/XN
    EventSource m_lazyAbstract;
    EventSource m_lazySocket;

    pid_t m_pid = 0;
    wl_client* m_client = nullptr;
    struct ClientListener {
        wl_listener listener;
        XwaylandServer* owner;
    } m_clientListener{};
    UniqueFd m_wmFd;       // our end of -wm
    UniqueFd m_displayFd;  // read end of -displayfd
    EventSource m_displayFdSource;
};

} // namespace dragon
