#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"

#include <cstdint>
#include <optional>
#include <string>

namespace dragon {

class Surface;

// A window the desktop manages, whatever protocol it speaks: xdg_toplevel
// (XdgWindow) or X11 through Xwayland (XwaylandWindow). Window management
// policy only ever sees this interface.
class Toplevel {
public:
    virtual ~Toplevel() = default;

    // The surface holding the window's content (null once it's gone).
    virtual Surface* surface() const = 0;
    // The "window" part of the surface (excludes client-side shadows), surface-local.
    virtual Rect geometry() const = 0;
    virtual std::string title() const = 0;
    virtual std::string appId() const = 0;
    virtual Size minSize() const = 0; // 0 = unconstrained
    virtual Size maxSize() const = 0;

    // State the desktop wants the client in (what it last asked for).
    virtual bool maximized() const = 0;
    virtual bool fullscreen() const = 0;

    virtual void setActivated(bool on) = 0;
    virtual void setSize(Size size) = 0; // 0x0: the client picks
    virtual void setMaximized(bool on) = 0;
    virtual void setFullscreen(bool on) = 0;
    virtual void setResizing(bool on) = 0;
    // Hidden from the screen (X11 clients track this; xdg has no such state).
    virtual void setMinimized(bool) {}
    virtual void setBounds(Size bounds) = 0;
    // Asks the client to close (it may refuse or ask the user first).
    virtual void close() = 0;
    // Title bars: can the compositor draw this window's (the client then
    // leaves its own out)? Wayland: clients that speak xdg-decoration; X11:
    // windows that don't ask for no decorations (Motif hints).
    virtual bool canServerDecorate() const { return false; }
    // Tells the client who draws the title bar.
    virtual void setServerDecorated(bool) {}

    // Where the desktop put the window's geometry (X11 clients position themselves by it).
    virtual void setPosition(Point) {}
    // The window went to the top of the stack.
    virtual void raised() {}

    Signal<> mapped;
    Signal<> unmapped;
    Signal<> committed;   // new content (resize anchoring follows this)
    Signal<> destroyed;   // gone for good: forget it
    // Interactive move/resize the client asked for. `serial` is the input event
    // it answers (Wayland); X11 sends none and relies on a held button.
    Signal<std::optional<std::uint32_t>> moveRequested;
    Signal<std::optional<std::uint32_t>, std::uint32_t> resizeRequested; // serial, xdg resize edges
    Signal<> minimizeRequested;
    Signal<bool> maximizeRequested;
    Signal<bool> fullscreenRequested;
    Signal<> titleChanged;
    Signal<> appIdChanged;
    Signal<> decorationChanged; // canServerDecorate() may differ now
};

} // namespace dragon
