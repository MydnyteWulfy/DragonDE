#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

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
    };

    // Where new windows get placed.
    void setArea(Rect area) { m_area = area; }

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

    Signal<Window*> windowMapped;
    Signal<Window*> focusChanged;
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

    Server& m_server;
    Window* m_focused = nullptr;

    Rect m_area{0, 0, 1280, 720};
    int m_cascade = 0;
    std::vector<std::unique_ptr<Window>> m_windows;
    std::vector<XdgPopup*> m_popups; // mapped, in mapping order
    std::unordered_map<const void*, std::unique_ptr<Tracked>> m_tracked;
    Connection m_toplevelCreated;
    Connection m_popupCreated;
};

} // namespace dragon
