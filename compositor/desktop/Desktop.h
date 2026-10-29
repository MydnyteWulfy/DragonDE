#pragma once

#include "config/Settings.h"
#include "core/Geometry.h"
#include "core/Signal.h"

#include <functional>
#include <memory>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

struct wl_client;

namespace dragon {

class LayerSurface;
class Server;
class Surface;
class Toplevel;
class XdgPopup;
class XdgSurface;
class XdgToplevel;
class XdgWindow;

// The set of mapped windows, popups and unmanaged surfaces, where they are
// and their stacking order, plus window management policy (focus, placement,
// maximize, popup grabs). Windows are Toplevels: xdg_toplevel or X11.
class Desktop {
public:
    explicit Desktop(Server& server);
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    struct Window {
        Toplevel* toplevel;
        Point position; // top-left of the window geometry, in global coordinates

        // Interactive resize: edges being dragged (XdgToplevelResizeEdge bits)
        // and the geometry when it started, so the opposite edges stay put.
        std::uint32_t resizeEdges = 0;
        Rect resizeStart;
        // Geometry to return to when leaving maximized/fullscreen.
        std::optional<Rect> restore;
        // Minimized: kept (and stacked) but not drawn or hit. Nonzero = when,
        // so the most recent one can be brought back first.
        std::uint64_t minimizedAt = 0;
        bool minimized() const { return minimizedAt != 0; }

        // How it looks (config + rules) and whether we draw its title bar.
        WindowStyle style = WindowStyle::plain();
        bool titlebar = false;
        // Title bar button under the pointer / held down (index into the layout).
        int hoveredButton = -1;
        int pressedButton = -1;

        std::vector<Connection> connections;
    };

    // Window styles: who gets which look (the config, with its rules).
    // Without one every window is WindowStyle::plain().
    using StyleResolver = std::function<WindowStyle(const Toplevel&)>;
    void setStyleResolver(StyleResolver resolver);
    // Re-resolves every window's and layer surface's style (after the config changed).
    void restyle();
    // Layer surface styles, by namespace (the config's layer rules).
    using LayerStyleResolver = std::function<LayerStyle(const LayerSurface&)>;
    void setLayerStyleResolver(LayerStyleResolver resolver);

    // A window's frame: what the compositor draws around and over it.
    struct Frame {
        Rect content;   // the client's window geometry
        Rect titlebar;  // ours, above the content (empty if none)
        Rect outer;     // content + title bar: clipped to `radius`, the border goes around it
        std::int32_t border = 0;
        float radius = 0;
        bool styled = false; // false: drawn exactly as the client made it (fullscreen, plain style)
        bool shadow = false;
    };
    Frame frameOf(const Window& window) const;

    // The parts of a frame the pointer can be over.
    struct DecorationHit {
        Window* window = nullptr;
        enum class Part { None, Titlebar, Button, Edge } part = Part::None;
        int button = -1;         // Button: index into layoutTitlebar(...).buttons
        std::string action;      // Button: minimize | maximize | close
        std::uint32_t edges = 0; // Edge: xdg resize edges
    };
    // The decoration at `point`, if nothing above it (a window, popup, X11
    // menu) is there instead.
    DecorationHit decorationAt(PointF point) const;

    // The screen. Layer surfaces (bars) may reserve parts of it; the rest is
    // the area where new windows get placed and what maximized windows fill.
    void setArea(Rect screen);
    Rect screen() const { return m_screen; }
    Rect area() const { return m_area; }

    // Layer shell surfaces: where one is (if mapped), and giving keyboard
    // focus to one that accepts it (a click). False if it doesn't.
    std::optional<Point> layerPosition(const LayerSurface* surface) const;
    bool focusLayer(Surface* surface);
    LayerSurface* layerFocus() const { return m_layerFocus; }

    // Adds a window from outside the xdg-shell path (X11). The desktop tracks
    // its map state from then on; the caller owns it.
    void addToplevel(Toplevel& toplevel);

    // Surfaces that place themselves and never take focus (X11 menus and
    // tooltips): drawn above windows at `position`, hit-testable.
    void setUnmanaged(Surface* surface, Point position);
    void removeUnmanaged(Surface* surface);

    // Global window geometry (position + window geometry size).
    Rect geometryOf(const Window& window) const;
    void moveWindow(Window& window, Point position);
    void beginResize(Window& window, std::uint32_t edges);
    void endResize(Window& window);
    void setMaximized(Window& window, bool on);
    void setFullscreen(Window& window, bool on);
    // Hides the window (focus moves to the next one down).
    void minimize(Window& window);
    // Shows a minimized window again, raised and focused.
    void unminimize(Window& window);
    // Brings back the most recently minimized window. False if there is none.
    bool restoreMinimized();

    // Mapped windows, bottom to top.
    const std::vector<std::unique_ptr<Window>>& windows() const { return m_windows; }
    Window* windowFor(const Toplevel* toplevel) const;

    // A root surface (window, popup, unmanaged) and where its surface origin
    // is. Each root's subsurfaces are drawn with it.
    struct RenderItem {
        Surface* surface;
        Point origin;
        const Window* window = nullptr; // set for a window's own surface: draw its frame
        const LayerSurface* layer = nullptr; // set for a layer surface's own surface
        const LayerStyle* layerStyle = nullptr;
        bool belowWindows = false; // wallpaper-level: background and bottom layers (and their menus)
    };
    // Bottom to top: each window followed by its popups, then unmanaged surfaces.
    std::vector<RenderItem> renderList() const;

    // The topmost surface accepting input at `point` (global coordinates),
    // honoring input regions and subsurfaces.
    struct Hit {
        Surface* surface = nullptr;
        PointF local; // surface-local position
    };
    Hit surfaceAt(PointF point) const;
    // Global position of any mapped surface's origin (windows, popups, subsurfaces, unmanaged).
    std::optional<Point> originOf(Surface* surface) const;
    // The window a surface belongs to (through subsurfaces and popups), if any.
    Window* windowOf(Surface* surface) const;

    // Raises the window and gives it keyboard focus (and the activated state).
    // A minimized window is shown again.
    void focus(Window* window);
    Window* focusedWindow() const { return m_focused; }

    // Popup grabs (menus): while one is active the topmost grabbing popup has
    // the keyboard, and input outside its client dismisses the whole chain.
    bool popupGrabActive() const { return !m_popupGrabs.empty(); }
    wl_client* popupGrabClient() const;
    void dismissPopups();

    Signal<Window*> windowMapped;
    Signal<Window*> windowUnmapped;
    Signal<Window*> focusChanged;
    Signal<Window*> windowVisibilityChanged; // minimized or restored
    Signal<> layersChanged;                  // a layer surface came, went or moved
    // The client asked to be moved/resized by the pointer (e.g. dragging its
    // title bar). Serial: the input event it answers (none for X11).
    Signal<Window*, std::optional<std::uint32_t>> moveRequested;
    Signal<Window*, std::optional<std::uint32_t>, std::uint32_t> resizeRequested; // + edges

private:
    struct Tracked;
    void track(Toplevel& toplevel);
    void trackPopup(XdgPopup* popup);
    void mapWindow(Toplevel& toplevel);
    void unmapWindow(Toplevel& toplevel);
    Window* windowForSurface(const Surface* surface) const;
    std::optional<Point> surfaceOrigin(const XdgSurface* xdg) const;
    static XdgToplevel* rootToplevel(const XdgPopup* popup);
    static Surface* rootSurface(Surface* surface); // up through subsurface parents
    static bool hitTree(Surface* surface, Point origin, PointF point, Hit& hit);
    void committed(Window& window);
    void popupMapped(XdgPopup* popup);
    void popupGone(XdgPopup* popup);
    void restoreKeyboardFocus();
    void setFillState(Window& window, bool on, bool fullscreen);
    Window* topmostVisible() const;
    void applyStyle(Toplevel& toplevel);
    struct LayerItem;
    void trackLayer(LayerSurface* surface);
    void arrangeLayers();
    void applyArea(Rect usable);
    void updateLayerFocus();
    void addLayer(std::vector<RenderItem>& items, std::uint32_t layer) const;
    static Surface* rootForeignParent(const XdgPopup* popup);
    std::int32_t titlebarHeight(const Window& window) const;

    Server& m_server;
    Window* m_focused = nullptr;

    Rect m_screen{0, 0, 1280, 720};
    Rect m_area{0, 0, 1280, 720}; // m_screen minus the layers' exclusive zones
    std::vector<std::unique_ptr<LayerItem>> m_layers; // in creation order
    LayerSurface* m_layerFocus = nullptr;
    Connection m_layerCreated;
    int m_cascade = 0;
    StyleResolver m_styleResolver;
    LayerStyleResolver m_layerStyleResolver;
    std::uint64_t m_minimizeCounter = 0;
    std::vector<std::unique_ptr<Window>> m_windows;
    std::vector<Toplevel*> m_toplevels;                  // all of them, mapped or not
    std::vector<std::unique_ptr<XdgWindow>> m_xdgWindows; // the ones we own
    std::vector<XdgPopup*> m_popups;                     // mapped, in mapping order
    std::vector<XdgPopup*> m_popupGrabs;                 // grabbing popups, topmost last
    struct Unmanaged {
        Surface* surface;
        Point position;
        Connection gone;
    };
    std::vector<std::unique_ptr<Unmanaged>> m_unmanaged; // bottom to top
    std::unordered_map<const void*, std::unique_ptr<Tracked>> m_tracked;
    Connection m_toplevelCreated;
    Connection m_popupCreated;
    Connection m_activation;
};

} // namespace dragon
