#pragma once

#include "core/Signal.h"
#include "wayland/Global.h"

#include <cstdint>
#include <memory>
#include <vector>

struct wl_display;

namespace dragon {

class EventLoop;
class Output;
class OutputGlobal;
class Surface;
class XdgPopup;
class XdgToplevel;

// The Wayland server side: owns the globals clients bind to and announces new
// client objects to the rest of the compositor (scene, window management).
class Server {
public:
    Server(wl_display* display, EventLoop& loop);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    wl_display* display() const { return m_display; }
    EventLoop& loop() const { return m_loop; }
    std::uint32_t nextSerial();

    void addOutput(Output& output);
    void removeOutput(Output& output);
    OutputGlobal* outputGlobal(const Output& output) const;

    Signal<Surface*> surfaceCreated;
    Signal<XdgToplevel*> toplevelCreated;
    Signal<XdgPopup*> popupCreated;

private:
    wl_display* m_display;
    EventLoop& m_loop;
    std::unique_ptr<Global> m_compositor;
    std::unique_ptr<Global> m_subcompositor;
    std::unique_ptr<Global> m_xdgWmBase;
    std::vector<std::unique_ptr<OutputGlobal>> m_outputs;
    Connection m_flush;
};

} // namespace dragon
