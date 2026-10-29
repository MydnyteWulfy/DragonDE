#pragma once

#include "core/Signal.h"
#include "wayland/Global.h"

#include "core/Geometry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct wl_client;
struct wl_display;
struct wl_global;

namespace dragon {

struct DmabufSupport;
class CursorShapeManager;
class EventLoop;
class LinuxDmabuf;
class Output;
class OutputGlobal;
class Seat;
class Surface;
class XdgActivation;
class XdgPopup;
class XdgSurface;
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

    // Advertises zwp_linux_dmabuf_v1 once the renderer knows what it can import.
    void enableDmabuf(DmabufSupport support);

    void addOutput(Output& output);
    void removeOutput(Output& output);
    OutputGlobal* outputGlobal(const Output& output) const;

    // Hides `global` from clients for which `allowed` returns false (e.g.
    // xwayland_shell_v1 is only for Xwayland).
    void restrictGlobal(const wl_global* global, std::function<bool(const wl_client*)> allowed);

    Seat& seat() const { return *m_seat; }
    XdgActivation& activation() const { return *m_activation; }

    // Where popups of `parent` must fit, in its window-geometry coordinates
    // (the desktop knows where things are; nullopt: unconstrained).
    std::function<std::optional<Rect>(const XdgSurface& parent)> popupBoundsProvider;
    std::optional<Rect> popupBounds(const XdgSurface& parent) const
    {
        return popupBoundsProvider ? popupBoundsProvider(parent) : std::nullopt;
    }

    Signal<Surface*> surfaceCreated;
    Signal<XdgToplevel*> toplevelCreated;
    Signal<XdgPopup*> popupCreated;

private:
    wl_display* m_display;
    EventLoop& m_loop;
    std::unique_ptr<Global> m_compositor;
    std::unique_ptr<Global> m_subcompositor;
    std::unique_ptr<Global> m_xdgWmBase;
    std::unique_ptr<LinuxDmabuf> m_dmabuf;
    std::unique_ptr<Seat> m_seat;
    std::unique_ptr<Global> m_dataDeviceManager;
    std::unique_ptr<Global> m_primarySelectionManager;
    std::unique_ptr<CursorShapeManager> m_cursorShapeManager;
    std::unique_ptr<XdgActivation> m_activation;
    std::vector<std::pair<const wl_global*, std::function<bool(const wl_client*)>>> m_globalFilters;
    std::vector<std::unique_ptr<OutputGlobal>> m_outputs;
    Connection m_flush;
};

} // namespace dragon
