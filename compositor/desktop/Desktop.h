#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct wl_client;

namespace dragon {

class Server;
class Surface;
class XdgPopup;
class XdgSurface;
class XdgToplevel;

// The set of mapped windows and popups, where they are and their stacking
// order. Window management policy (focus, move, tiling) grows from here.
class Desktop {
public:
    explicit Desktop(Server& server);
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    struct Window {
        XdgToplevel* toplevel;
        Point position; // top-left of the window geometry, in global coordinates

        // Interactive resize: edges being dragged (XdgToplevelResizeEdge bits)
        // and the geometry when it started, so the opposite edges stay put.
        std::uint32_t resizeEdges = 0;
        Rect resizeStart;
        // Geometry to return to when leaving maximized/fullscreen.
        std::optional<Rect> restore;

        std::vector<Connection> connections;
    };

    // Where new windows get placed and what maximized windows fill.
    void setArea(Rect area);
    Rect area() const { return m_area; }

    // Global window geometry (position + xdg window geometry size).
    Rect geometryOf(const Window& window) const;
    void beginResize(Window& window, std::uint32_t edges);
    void endResize(Window& window);
    void setMaximized(Window& window, bool on);
    void setFullscreen(Window& window, bool on);

    // Mapped windows, bottom to top.
    const std::vector<std::unique_ptr<Window>>& windows() const { return m_windows; }
    Window* windowFor(const XdgToplevel* toplevel) const;

    // A root surface (window or popup) and where its surface origin is. Each
    // root's subsurfaces are drawn with it.
    struct RenderItem {
        Surface* surface;
        Point origin;
    };
    // Bottom to top; each window is followed by its popups.
    std::vector<RenderItem> renderList() const;

    // The topmost surface accepting input at `point` (global coordinates),
    // honoring input regions and subsurfaces.
    struct Hit {
        Surface* surface = nullptr;
        PointF local; // surface-local position
    };
    Hit surfaceAt(PointF point) const;
    // Global position of any mapped surface's origin (windows, popups, subsurfaces).
    std::optional<Point> originOf(Surface* surface) const;
    // The window a surface belongs to (through subsurfaces and popups), if any.
    Window* windowOf(Surface* surface) const;

    // Raises the window and gives it keyboard focus (and the activated state).
    void focus(Window* window);
    Window* focusedWindow() const { return m_focused; }

    // Popup grabs (menus): while one is active the topmost grabbing popup has
    // the keyboard, and input outside its client dismisses the whole chain.
    bool popupGrabActive() const { return !m_popupGrabs.empty(); }
    wl_client* popupGrabClient() const;
    void dismissPopups();

    Signal<Window*> windowMapped;
    Signal<Window*> focusChanged;
    // The client asked to be moved/resized by the pointer (e.g. dragging its title bar).
    Signal<Window*, std::uint32_t> moveRequested;                // serial
    Signal<Window*, std::uint32_t, std::uint32_t> resizeRequested; // serial, edges
    Signal<Window*> windowUnmapped;

private:
    struct Tracked;
    void trackToplevel(XdgToplevel* toplevel);
    void trackPopup(XdgPopup* popup);
    void mapWindow(XdgToplevel* toplevel);
    void unmapWindow(XdgToplevel* toplevel);
    std::optional<Point> surfaceOrigin(const XdgSurface* xdg) const;
    static XdgToplevel* rootToplevel(const XdgPopup* popup);
    static Surface* rootSurface(Surface* surface); // up through subsurface parents
    static bool hitTree(Surface* surface, Point origin, PointF point, Hit& hit);
    void committed(Window& window);
    void popupMapped(XdgPopup* popup);
    void popupGone(XdgPopup* popup);
    void restoreKeyboardFocus();
    void setFillState(Window& window, bool on, bool fullscreen);

    Server& m_server;
    Window* m_focused = nullptr;

    Rect m_area{0, 0, 1280, 720};
    int m_cascade = 0;
    std::vector<std::unique_ptr<Window>> m_windows;
    std::vector<XdgToplevel*> m_toplevels; // all of them, mapped or not
    std::vector<XdgPopup*> m_popups;     // mapped, in mapping order
    std::vector<XdgPopup*> m_popupGrabs; // grabbing popups, topmost last
    std::unordered_map<const void*, std::unique_ptr<Tracked>> m_tracked;
    Connection m_toplevelCreated;
    Connection m_popupCreated;
    Connection m_activation;
};

} // namespace dragon
