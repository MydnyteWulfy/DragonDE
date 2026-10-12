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

    Signal<Window*> windowMapped;
    Signal<Window*> windowUnmapped;

private:
    struct Tracked;
    void trackToplevel(XdgToplevel* toplevel);
    void trackPopup(XdgPopup* popup);
    void mapWindow(XdgToplevel* toplevel);
    void unmapWindow(XdgToplevel* toplevel);
    std::optional<Point> surfaceOrigin(const XdgSurface* xdg) const;
    static XdgToplevel* rootToplevel(const XdgPopup* popup);

    Rect m_area{0, 0, 1280, 720};
    int m_cascade = 0;
    std::vector<std::unique_ptr<Window>> m_windows;
    std::vector<XdgPopup*> m_popups; // mapped, in mapping order
    std::unordered_map<const void*, std::unique_ptr<Tracked>> m_tracked;
    Connection m_toplevelCreated;
    Connection m_popupCreated;
};

} // namespace dragon
