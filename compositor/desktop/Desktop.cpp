#include "desktop/Desktop.h"

#include "Server.h"
#include "core/Log.h"
#include "desktop/Titlebar.h"
#include "desktop/Toplevel.h"
#include "desktop/XdgWindow.h"
#include "protocols/Compositor.h"
#include "protocols/LayerShell.h"
#include "protocols/Seat.h"
#include "protocols/XdgActivation.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <cmath>

namespace dragon {

// Signal connections for one toplevel or popup, dropped when it's destroyed.
struct Desktop::LayerItem {
    LayerSurface* surface;
    Point position;
    std::vector<Connection> connections;
    LayerStyle style;
};

struct Desktop::Tracked {
    std::vector<Connection> connections;
    // Toplevels only: their look, known before they map.
    WindowStyle style = WindowStyle::plain();
    bool titlebar = false;
};

namespace {
// Grab area for resizing, outside the border (borders are often 1-2px).
constexpr std::int32_t kResizeMargin = 6;
// How far from a corner a side edge still resizes diagonally.
constexpr std::int32_t kCornerSize = 20;
constexpr std::uint32_t kEdgeTop = 1, kEdgeBottom = 2, kEdgeLeft = 4, kEdgeRight = 8; // xdg resize edge bits
} // namespace

Desktop::Desktop(Server& server) : m_server(server)
{
    m_toplevelCreated = server.toplevelCreated.connect([this](XdgToplevel* t) {
        m_xdgWindows.push_back(std::make_unique<XdgWindow>(*t));
        track(*m_xdgWindows.back());
    });
    m_popupCreated = server.popupCreated.connect([this](XdgPopup* p) { trackPopup(p); });
    m_layerCreated = server.layerShell().surfaceCreated.connect([this](LayerSurface* s) { trackLayer(s); });

    m_activation = server.activation().activationRequested.connect([this](Surface* surface) {
        if (Window* window = windowOf(surface)) {
            log::debug("desktop", "activating '{}'", window->toplevel->title());
            focus(window);
        }
    });

    // Popups must fit in the area: express it relative to the parent's window geometry.
    server.popupBoundsProvider = [this](const XdgSurface& parent) -> std::optional<Rect> {
        auto origin = surfaceOrigin(&parent);
        if (!origin)
            return std::nullopt;
        const Point g = *origin + parent.geometry().position();
        return Rect{m_screen.x - g.x, m_screen.y - g.y, m_screen.width, m_screen.height};
    };
    server.surfacePopupBoundsProvider = [this](const Surface& parent) -> std::optional<Rect> {
        auto origin = originOf(const_cast<Surface*>(&parent));
        if (!origin)
            return std::nullopt;
        return Rect{m_screen.x - origin->x, m_screen.y - origin->y, m_screen.width, m_screen.height};
    };
}

Desktop::~Desktop()
{
    m_server.popupBoundsProvider = nullptr;
    m_server.surfacePopupBoundsProvider = nullptr;
    m_layers.clear();
    m_tracked.clear(); // stop listening before our containers go
}

Desktop::Window* Desktop::windowFor(const Toplevel* toplevel) const
{
    auto it = std::ranges::find_if(m_windows, [toplevel](const auto& w) { return w->toplevel == toplevel; });
    return it == m_windows.end() ? nullptr : it->get();
}

Desktop::Window* Desktop::windowForSurface(const Surface* surface) const
{
    if (!surface)
        return nullptr;
    auto it = std::ranges::find_if(m_windows, [surface](const auto& w) { return w->toplevel->surface() == surface; });
    return it == m_windows.end() ? nullptr : it->get();
}

void Desktop::addToplevel(Toplevel& toplevel)
{
    track(toplevel);
}

void Desktop::track(Toplevel& toplevel)
{
    Toplevel* t = &toplevel;
    t->setBounds(m_area.size()); // so the first configure already tells the client what fits
    m_toplevels.push_back(t);
    auto tracked = std::make_unique<Tracked>();
    tracked->connections.push_back(t->mapped.connect([this, t] { mapWindow(*t); }));
    tracked->connections.push_back(t->unmapped.connect([this, t] { unmapWindow(*t); }));
    // Before mapping there's no Window yet: answer in the first configure.
    tracked->connections.push_back(t->maximizeRequested.connect([this, t](bool on) {
        if (Window* w = windowFor(t))
            setMaximized(*w, on);
        else {
            const auto it = m_tracked.find(t);
            const std::int32_t th = it != m_tracked.end() && it->second->titlebar ? it->second->style.titlebar.height : 0;
            t->setMaximized(on);
            t->setSize(on ? Size{m_area.width, m_area.height - th} : Size{});
        }
    }));
    tracked->connections.push_back(t->minimizeRequested.connect([this, t] {
        if (Window* w = windowFor(t))
            minimize(*w);
    }));
    tracked->connections.push_back(t->fullscreenRequested.connect([this, t](bool on) {
        if (Window* w = windowFor(t))
            setFullscreen(*w, on);
        else {
            t->setFullscreen(on);
            t->setSize(on ? m_screen.size() : Size{});
        }
    }));
    tracked->connections.push_back(t->destroyed.connect([this, t] {
        unmapWindow(*t); // no-op if already unmapped
        std::erase(m_toplevels, t);
        m_tracked.erase(t); // destroys this connection's owner: nothing below may touch `tracked`
        std::erase_if(m_xdgWindows, [t](const auto& w) { return w.get() == t; });
    }));
    for (Signal<>* changed : {&t->titleChanged, &t->appIdChanged, &t->decorationChanged})
        tracked->connections.push_back(changed->connect([this, t] { applyStyle(*t); }));
    m_tracked.emplace(t, std::move(tracked));
    applyStyle(*t);
}

void Desktop::setStyleResolver(StyleResolver resolver)
{
    m_styleResolver = std::move(resolver);
    restyle();
}

void Desktop::restyle()
{
    for (Toplevel* t : std::vector(m_toplevels)) // applyStyle may refit windows, nothing more
        applyStyle(*t);
    for (auto& l : m_layers)
        l->style = m_layerStyleResolver ? m_layerStyleResolver(*l->surface) : LayerStyle{};
}

void Desktop::setLayerStyleResolver(LayerStyleResolver resolver)
{
    m_layerStyleResolver = std::move(resolver);
    restyle();
}

void Desktop::applyStyle(Toplevel& toplevel)
{
    const auto it = m_tracked.find(&toplevel);
    if (it == m_tracked.end())
        return;
    Tracked& tracked = *it->second;
    tracked.style = m_styleResolver ? m_styleResolver(toplevel) : WindowStyle::plain();
    tracked.titlebar = tracked.style.titlebar.enabled && toplevel.canServerDecorate();
    toplevel.setServerDecorated(tracked.titlebar);

    Window* w = windowFor(&toplevel);
    if (!w)
        return;
    const std::int32_t oldHeight = titlebarHeight(*w);
    w->style = tracked.style;
    w->titlebar = tracked.titlebar;
    if (titlebarHeight(*w) == oldHeight)
        return;
    w->hoveredButton = w->pressedButton = -1;
    if (w->restore) {
        setFillState(*w, true, toplevel.fullscreen()); // refit under the new title bar
        return;
    }
    // A title bar appeared: keep it on screen.
    const Frame f = frameOf(*w);
    if (f.outer.y - f.border < m_area.y)
        moveWindow(*w, {w->position.x, w->position.y + (m_area.y - (f.outer.y - f.border))});
}

std::int32_t Desktop::titlebarHeight(const Window& window) const
{
    return window.titlebar && !window.toplevel->fullscreen() ? window.style.titlebar.height : 0;
}

Desktop::Frame Desktop::frameOf(const Window& window) const
{
    Frame f;
    f.content = geometryOf(window);
    const std::int32_t th = titlebarHeight(window);
    f.titlebar = {f.content.x, f.content.y - th, f.content.width, th};
    f.outer = {f.content.x, f.content.y - th, f.content.width, f.content.height + th};
    if (window.toplevel->fullscreen())
        return f; // nothing: games and videos get every pixel
    f.styled = window.style.styled();
    if (!window.toplevel->maximized()) {
        f.border = window.style.border.width;
        f.radius = float(window.style.cornerRadius);
        f.shadow = window.style.shadow.enabled;
    }
    return f;
}

Desktop::DecorationHit Desktop::decorationAt(PointF point) const
{
    // Whatever surface is there wins if its window is at least as high in the stack.
    const Hit hit = surfaceAt(point);
    std::ptrdiff_t surfaceLevel = -1;
    if (hit.surface) {
        const Window* owner = windowOf(hit.surface);
        auto it = std::ranges::find_if(m_windows, [owner](const auto& w) { return w.get() == owner; });
        surfaceLevel = owner && it != m_windows.end() ? it - m_windows.begin() : std::ptrdiff_t(m_windows.size());
    }

    for (auto i = std::ptrdiff_t(m_windows.size()) - 1; i > surfaceLevel; --i) {
        Window& w = *m_windows[std::size_t(i)];
        if (w.minimized())
            continue;
        const Frame f = frameOf(w);
        if (f.content.contains(point))
            return {}; // the window is there, even where it takes no input
        if (!f.styled)
            continue;
        const bool resizable = !w.toplevel->maximized() && !w.toplevel->fullscreen();
        const Rect zone = resizable ? f.outer.adjusted(f.border + kResizeMargin) : f.outer;
        if (!zone.contains(point))
            continue;

        DecorationHit d{.window = &w};
        if (f.titlebar.contains(point)) {
            const auto layout = layoutTitlebar(w.style.titlebar, f.titlebar.width);
            const PointF local{point.x - f.titlebar.x, point.y - f.titlebar.y};
            for (std::size_t b = 0; b < layout.buttons.size(); ++b) {
                if (layout.buttons[b].rect.contains(local)) {
                    d.part = DecorationHit::Part::Button;
                    d.button = int(b);
                    d.action = layout.buttons[b].action;
                    return d;
                }
            }
            d.part = DecorationHit::Part::Titlebar;
            return d;
        }
        if (!resizable)
            return {}; // the outer area of a maximized window: nothing to grab
        // Outside the frame: an edge, or a corner within kCornerSize of one.
        const Rect o = f.outer;
        const bool left = point.x < o.x, right = point.x >= o.x + o.width;
        const bool top = point.y < o.y, bottom = point.y >= o.y + o.height;
        const bool nearLeft = point.x < o.x + kCornerSize, nearRight = point.x >= o.x + o.width - kCornerSize;
        const bool nearTop = point.y < o.y + kCornerSize, nearBottom = point.y >= o.y + o.height - kCornerSize;
        if (top || (nearTop && (left || right)))
            d.edges |= kEdgeTop;
        if (bottom || (nearBottom && (left || right)))
            d.edges |= kEdgeBottom;
        if (left || (nearLeft && (top || bottom)))
            d.edges |= kEdgeLeft;
        if (right || (nearRight && (top || bottom)))
            d.edges |= kEdgeRight;
        d.part = DecorationHit::Part::Edge;
        return d;
    }
    return {};
}

void Desktop::trackPopup(XdgPopup* popup)
{
    auto tracked = std::make_unique<Tracked>();
    XdgSurface* xdg = popup->xdgSurface();
    tracked->connections.push_back(xdg->mapped.connect([this, popup] { popupMapped(popup); }));
    tracked->connections.push_back(xdg->unmapped.connect([this, popup] { popupGone(popup); }));
    tracked->connections.push_back(popup->destroyed.connect([this, popup] {
        popupGone(popup);
        m_tracked.erase(popup);
    }));
    m_tracked.emplace(popup, std::move(tracked));
}

void Desktop::mapWindow(Toplevel& toplevel)
{
    if (windowFor(&toplevel))
        return;
    Rect geometry = toplevel.geometry();
    const auto tracked = m_tracked.find(&toplevel);
    const WindowStyle style = tracked != m_tracked.end() ? tracked->second->style : WindowStyle::plain();
    const bool titlebar = tracked != m_tracked.end() && tracked->second->titlebar;

    // Room the frame takes: title bar on top, border all around.
    const std::int32_t b = style.border.width, th = titlebar ? style.titlebar.height : 0;
    const Size room{m_area.width - 2 * b, m_area.height - 2 * b - th};

    // Bigger than the screen (clients that ignore configure_bounds, like GTK3
    // dialogs): ask it to fit, within what its minimum size allows. It's
    // placed as if it already had the new size.
    const Size min = toplevel.minSize();
    if (geometry.width > room.width || geometry.height > room.height) {
        const Size fit{std::max(std::min(geometry.width, room.width), min.width),
                       std::max(std::min(geometry.height, room.height), min.height)};
        if (fit != geometry.size()) {
            toplevel.setSize(fit);
            geometry.width = fit.width;
            geometry.height = fit.height;
        }
    }

    // Centered, cascading so new windows don't hide each other exactly.
    const std::int32_t offset = m_cascade * 32;
    m_cascade = (m_cascade + 1) % 8;
    Point pos{m_area.x + (room.width - geometry.width) / 2 + b + offset,
              m_area.y + (room.height - geometry.height) / 2 + b + th + offset};
    pos.x = std::max(pos.x, m_area.x + b);
    pos.y = std::max(pos.y, m_area.y + b + th);
    if (toplevel.fullscreen())
        pos = m_screen.position(); // started fullscreen/maximized: fill the screen/area
    else if (toplevel.maximized())
        pos = {m_area.x, m_area.y + th};

    m_windows.push_back(
        std::make_unique<Window>(Window{.toplevel = &toplevel, .position = pos, .style = style, .titlebar = titlebar}));
    Window* window = m_windows.back().get();
    toplevel.setPosition(pos);
    window->connections.push_back(toplevel.moveRequested.connect(
        [this, window](std::optional<std::uint32_t> serial) { moveRequested.emit(window, serial); }));
    window->connections.push_back(toplevel.resizeRequested.connect(
        [this, window](std::optional<std::uint32_t> serial, std::uint32_t edges) {
            resizeRequested.emit(window, serial, edges);
        }));
    window->connections.push_back(toplevel.committed.connect([this, window] { committed(*window); }));
    log::debug("desktop", "mapped '{}' ({}) {}x{} at {},{}", toplevel.title(), toplevel.appId(), geometry.width,
               geometry.height, pos.x, pos.y);
    windowMapped.emit(window);
    focus(window); // new windows get focus
}

void Desktop::unmapWindow(Toplevel& toplevel)
{
    auto it = std::ranges::find_if(m_windows, [&toplevel](const auto& w) { return w->toplevel == &toplevel; });
    if (it == m_windows.end())
        return;
    auto window = std::move(*it);
    m_windows.erase(it);
    log::debug("desktop", "unmapped '{}'", toplevel.title());
    windowUnmapped.emit(window.get());
    if (m_focused == window.get()) {
        m_focused = nullptr;
        focus(topmostVisible()); // next one down
    }
}

Desktop::Window* Desktop::topmostVisible() const
{
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        if (!(*it)->minimized())
            return it->get();
    }
    return nullptr;
}

void Desktop::minimize(Window& window)
{
    if (window.minimized())
        return;
    if (m_focused == &window && !m_popupGrabs.empty())
        dismissPopups();
    window.minimizedAt = ++m_minimizeCounter;
    window.toplevel->setMinimized(true);
    log::debug("desktop", "minimized '{}'", window.toplevel->title());
    if (m_focused == &window)
        focus(topmostVisible());
    windowVisibilityChanged.emit(&window);
}

void Desktop::unminimize(Window& window)
{
    if (window.minimized()) {
        window.minimizedAt = 0;
        window.toplevel->setMinimized(false);
        log::debug("desktop", "restored '{}'", window.toplevel->title());
        windowVisibilityChanged.emit(&window);
    }
    focus(&window);
}

bool Desktop::restoreMinimized()
{
    Window* latest = nullptr;
    for (const auto& w : m_windows) {
        if (w->minimized() && (!latest || w->minimizedAt > latest->minimizedAt))
            latest = w.get();
    }
    if (!latest)
        return false;
    unminimize(*latest);
    return true;
}

void Desktop::setUnmanaged(Surface* surface, Point position)
{
    auto it = std::ranges::find_if(m_unmanaged, [surface](const auto& u) { return u->surface == surface; });
    if (it != m_unmanaged.end()) {
        (*it)->position = position;
        return;
    }
    auto u = std::make_unique<Unmanaged>(Unmanaged{surface, position, {}});
    u->gone = surface->aboutToBeDestroyed.connect([this, surface] { removeUnmanaged(surface); });
    m_unmanaged.push_back(std::move(u));
}

void Desktop::removeUnmanaged(Surface* surface)
{
    std::erase_if(m_unmanaged, [surface](const auto& u) { return u->surface == surface; });
}

XdgToplevel* Desktop::rootToplevel(const XdgPopup* popup)
{
    for (const XdgSurface* parent = popup->parent(); parent;) {
        if (parent->toplevel())
            return parent->toplevel();
        if (!parent->popup())
            return nullptr;
        parent = parent->popup()->parent();
    }
    return nullptr;
}

std::optional<Point> Desktop::surfaceOrigin(const XdgSurface* xdg) const
{
    const Point geometryOffset = xdg->geometry().position();
    if (xdg->toplevel()) {
        const Window* w = windowForSurface(xdg->surface());
        if (!w)
            return std::nullopt;
        return w->position - geometryOffset;
    }
    if (const XdgPopup* popup = xdg->popup()) {
        const XdgSurface* parent = popup->parent();
        if (!parent) {
            // A layer surface's popup: relative to that surface's origin.
            Surface* foreign = popup->foreignParent();
            auto origin = foreign ? originOf(foreign) : std::nullopt;
            if (!origin)
                return std::nullopt;
            return *origin + popup->geometry().position() - geometryOffset;
        }
        auto parentOrigin = surfaceOrigin(parent);
        if (!parentOrigin)
            return std::nullopt;
        // Popup geometry is relative to the parent's window geometry.
        return *parentOrigin + parent->geometry().position() + popup->geometry().position() - geometryOffset;
    }
    return std::nullopt;
}

std::vector<Desktop::RenderItem> Desktop::renderList() const
{
    using Layer = LayerSurface::Layer;
    std::vector<RenderItem> items;
    addLayer(items, std::uint32_t(Layer::Background));
    addLayer(items, std::uint32_t(Layer::Bottom));
    // A fullscreen window on top covers bars too; only overlays stay above it.
    const Window* top = topmostVisible();
    const bool fullscreenOnTop = top && top->toplevel->fullscreen();
    if (fullscreenOnTop)
        addLayer(items, std::uint32_t(Layer::Top));
    for (const auto& w : m_windows) {
        Surface* surface = w->toplevel->surface();
        if (!surface || w->minimized())
            continue;
        items.push_back({surface, w->position - w->toplevel->geometry().position(), w.get()});

        for (XdgPopup* popup : m_popups) {
            const XdgToplevel* root = rootToplevel(popup);
            if (!root || !root->xdgSurface() || root->xdgSurface()->surface() != surface || !popup->xdgSurface() ||
                !popup->xdgSurface()->surface())
                continue;
            if (auto origin = surfaceOrigin(popup->xdgSurface()))
                items.push_back({popup->xdgSurface()->surface(), *origin});
        }
    }
    if (!fullscreenOnTop)
        addLayer(items, std::uint32_t(Layer::Top));
    for (const auto& u : m_unmanaged) {
        if (u->surface->hasContent())
            items.push_back({u->surface, u->position});
    }
    addLayer(items, std::uint32_t(Layer::Overlay));
    return items;
}

void Desktop::addLayer(std::vector<RenderItem>& items, std::uint32_t layer) const
{
    for (const auto& l : m_layers) {
        Surface* surface = l->surface->surface();
        if (!surface || !l->surface->isMapped() || std::uint32_t(l->surface->state().layer) != layer)
            continue;
        const bool below = layer <= std::uint32_t(LayerSurface::Layer::Bottom);
        items.push_back({.surface = surface, .origin = l->position, .layer = l->surface, .layerStyle = &l->style,
                         .belowWindows = below});
        for (XdgPopup* popup : m_popups) { // its menus, right above it
            if (rootForeignParent(popup) != surface || !popup->xdgSurface() || !popup->xdgSurface()->surface())
                continue;
            if (auto origin = surfaceOrigin(popup->xdgSurface()))
                items.push_back({.surface = popup->xdgSurface()->surface(), .origin = *origin, .belowWindows = below});
        }
    }
}

Surface* Desktop::rootForeignParent(const XdgPopup* popup)
{
    while (popup) {
        if (!popup->parent())
            return popup->foreignParent();
        popup = popup->parent()->popup(); // null when the chain ends at a toplevel
    }
    return nullptr;
}

// ------------------------------------------------------------ layer shell --

void Desktop::trackLayer(LayerSurface* surface)
{
    auto item = std::make_unique<LayerItem>(LayerItem{surface, {}, {}, {}});
    if (m_layerStyleResolver)
        item->style = m_layerStyleResolver(*surface);
    item->connections.push_back(surface->stateChanged.connect([this] { arrangeLayers(); }));
    item->connections.push_back(surface->mapped.connect([this, surface] {
        log::debug("desktop", "layer surface '{}' mapped in layer {}", surface->nameSpace(),
                   std::uint32_t(surface->state().layer));
        arrangeLayers();
        updateLayerFocus();
        layersChanged.emit();
    }));
    item->connections.push_back(surface->unmapped.connect([this, surface] {
        for (XdgPopup* popup : std::vector(m_popups)) {
            if (rootForeignParent(popup) == surface->surface())
                popup->dismiss();
        }
        arrangeLayers();
        updateLayerFocus();
        layersChanged.emit();
    }));
    item->connections.push_back(surface->destroyed.connect([this, surface] {
        if (m_layerFocus == surface)
            m_layerFocus = nullptr;
        std::erase_if(m_layers, [surface](const auto& l) { return l->surface == surface; }); // drops this connection
        arrangeLayers();
        updateLayerFocus();
        layersChanged.emit();
    }));
    m_layers.push_back(std::move(item));
}

void Desktop::arrangeLayers()
{
    using Layer = LayerSurface::Layer;
    constexpr auto kTop = LayerSurface::kTop, kBottom = LayerSurface::kBottom, kLeft = LayerSurface::kLeft,
                   kRight = LayerSurface::kRight;

    // Places `l` inside `bounds` by its anchors, size and margins; tells it its size.
    auto place = [](LayerItem& l, Rect bounds) {
        const auto& s = l.surface->state();
        Size size = s.size;
        const bool horizontal = (s.anchor & (kLeft | kRight)) == (kLeft | kRight);
        const bool vertical = (s.anchor & (kTop | kBottom)) == (kTop | kBottom);
        if (size.width == 0)
            size.width = bounds.width - s.marginLeft - s.marginRight;
        if (size.height == 0)
            size.height = bounds.height - s.marginTop - s.marginBottom;

        std::int32_t x = 0, y = 0;
        if (horizontal)
            x = bounds.x + s.marginLeft + (bounds.width - s.marginLeft - s.marginRight - size.width) / 2;
        else if (s.anchor & kLeft)
            x = bounds.x + s.marginLeft;
        else if (s.anchor & kRight)
            x = bounds.x + bounds.width - s.marginRight - size.width;
        else
            x = bounds.x + (bounds.width - size.width) / 2;
        if (vertical)
            y = bounds.y + s.marginTop + (bounds.height - s.marginTop - s.marginBottom - size.height) / 2;
        else if (s.anchor & kTop)
            y = bounds.y + s.marginTop;
        else if (s.anchor & kBottom)
            y = bounds.y + bounds.height - s.marginBottom - size.height;
        else
            y = bounds.y + (bounds.height - size.height) / 2;

        if (size.width <= 0 || size.height <= 0) {
            log::debug("desktop", "no room for layer surface '{}': closing it", l.surface->nameSpace());
            l.surface->close();
            return;
        }
        l.position = {x, y};
        l.surface->configure(size);
    };

    // Surfaces reserving space go first, top layers first, each taking its
    // strip out of what's left; then everything else goes in the remainder.
    Rect usable = m_screen;
    const Layer order[] = {Layer::Overlay, Layer::Top, Layer::Bottom, Layer::Background};
    for (Layer layer : order) {
        for (auto& l : m_layers) {
            const auto& s = l->surface->state();
            if (!l->surface->initialized() || s.layer != layer || s.exclusiveZone <= 0)
                continue;
            place(*l, usable);
            if (!l->surface->isMapped())
                continue; // reserves nothing until it's on screen
            const std::int32_t zone = s.exclusiveZone;
            switch (l->surface->exclusiveEdge()) {
            case kTop: usable.y += zone + s.marginTop; usable.height -= zone + s.marginTop; break;
            case kBottom: usable.height -= zone + s.marginBottom; break;
            case kLeft: usable.x += zone + s.marginLeft; usable.width -= zone + s.marginLeft; break;
            case kRight: usable.width -= zone + s.marginRight; break;
            default: break;
            }
        }
    }
    for (Layer layer : order) {
        for (auto& l : m_layers) {
            const auto& s = l->surface->state();
            if (l->surface->initialized() && s.layer == layer && s.exclusiveZone <= 0)
                place(*l, s.exclusiveZone < 0 ? m_screen : usable);
        }
    }
    usable.width = std::max(usable.width, 1);
    usable.height = std::max(usable.height, 1);
    applyArea(usable);
}

std::optional<Point> Desktop::layerPosition(const LayerSurface* surface) const
{
    for (const auto& l : m_layers) {
        if (l->surface == surface && surface->isMapped())
            return l->position;
    }
    return std::nullopt;
}

void Desktop::updateLayerFocus()
{
    using Keyboard = LayerSurface::Keyboard;
    using Layer = LayerSurface::Layer;
    // An exclusive surface in the top or overlay layer (a launcher, a lock
    // screen) has the keyboard while it's up; the newest, highest one wins.
    LayerSurface* exclusive = nullptr;
    for (Layer layer : {Layer::Top, Layer::Overlay}) {
        for (const auto& l : m_layers) {
            const auto& s = l->surface->state();
            if (l->surface->isMapped() && s.layer == layer && s.keyboard == Keyboard::Exclusive)
                exclusive = l->surface;
        }
    }
    if (exclusive)
        m_layerFocus = exclusive;
    else if (m_layerFocus && (!m_layerFocus->isMapped() || m_layerFocus->state().keyboard != Keyboard::OnDemand))
        m_layerFocus = nullptr;
    restoreKeyboardFocus();
}

bool Desktop::focusLayer(Surface* surface)
{
    Surface* root = rootSurface(surface);
    for (const auto& l : m_layers) {
        if (l->surface->surface() != root || !l->surface->isMapped())
            continue;
        if (l->surface->state().keyboard == LayerSurface::Keyboard::None)
            return false;
        if (m_layerFocus && m_layerFocus->state().keyboard == LayerSurface::Keyboard::Exclusive)
            return false; // an exclusive one keeps it
        m_layerFocus = l->surface;
        dismissPopups();
        restoreKeyboardFocus();
        return true;
    }
    return false;
}

void Desktop::setArea(Rect screen)
{
    if (screen == m_screen)
        return;
    m_screen = screen;
    arrangeLayers();
    // Fullscreen windows follow the screen even when the usable area stays the same.
    for (const auto& w : m_windows) {
        if (w->toplevel->fullscreen())
            setFillState(*w, true, true);
    }
}

void Desktop::applyArea(Rect area)
{
    if (area == m_area)
        return;
    m_area = area;
    for (Toplevel* t : m_toplevels)
        t->setBounds(area.size());
    for (const auto& w : m_windows) {
        if (w->restore) // maximized/fullscreen windows follow the area
            setFillState(*w, true, w->toplevel->fullscreen());
    }
}

Rect Desktop::geometryOf(const Window& window) const
{
    const Size size = window.toplevel->geometry().size();
    return {window.position.x, window.position.y, size.width, size.height};
}

void Desktop::moveWindow(Window& window, Point position)
{
    window.position = position;
    window.toplevel->setPosition(position);
}

void Desktop::beginResize(Window& window, std::uint32_t edges)
{
    window.resizeEdges = edges;
    window.resizeStart = geometryOf(window);
    window.toplevel->setResizing(true);
}

void Desktop::endResize(Window& window)
{
    window.resizeEdges = 0;
    window.toplevel->setResizing(false);
}

void Desktop::committed(Window& window)
{
    if (!window.resizeEdges)
        return;
    // Dragging the left/top edge: the client grew or shrank from the right/
    // bottom, so move the window to keep the opposite edge where it was.
    const Rect g = geometryOf(window);
    const auto edges = window.resizeEdges;
    Point p = window.position;
    if (edges & std::uint32_t(proto::XdgToplevelResizeEdge::Left))
        p.x = window.resizeStart.x + window.resizeStart.width - g.width;
    if (edges & std::uint32_t(proto::XdgToplevelResizeEdge::Top))
        p.y = window.resizeStart.y + window.resizeStart.height - g.height;
    if (p != window.position)
        moveWindow(window, p);
}

void Desktop::setMaximized(Window& window, bool on)
{
    setFillState(window, on, /*fullscreen=*/false);
}

void Desktop::setFullscreen(Window& window, bool on)
{
    setFillState(window, on, /*fullscreen=*/true);
}

void Desktop::setFillState(Window& window, bool on, bool fullscreen)
{
    Toplevel* tl = window.toplevel;
    if (fullscreen)
        tl->setFullscreen(on);
    else
        tl->setMaximized(on);

    if (tl->maximized() || tl->fullscreen()) {
        if (!window.restore)
            window.restore = geometryOf(window);
        // Fullscreen covers the screen; maximized windows fill the area (what
        // bars leave free) below their title bar.
        Rect target = tl->fullscreen() ? m_screen : m_area;
        if (!tl->fullscreen()) {
            const std::int32_t th = titlebarHeight(window);
            target.y += th;
            target.height -= th;
        }
        tl->setSize(target.size());
        moveWindow(window, target.position());
    } else if (window.restore) {
        tl->setSize(window.restore->size());
        moveWindow(window, window.restore->position());
        window.restore.reset();
    }
}

void Desktop::popupMapped(XdgPopup* popup)
{
    m_popups.push_back(popup);
    if (!popup->wantsGrab())
        return;

    // A grab needs real user input behind it, and must nest: its parent is the
    // topmost grabbing popup, or the focused window when there's none.
    XdgSurface* xdg = popup->xdgSurface();
    Surface* surface = xdg ? xdg->surface() : nullptr;
    const bool fromInput = surface && m_server.seat().isRecentInputSerial(surface->client(), popup->grabSerial());
    const XdgToplevel* root = rootToplevel(popup);
    // (Or, for a bar's menu, a layer surface.)
    const bool ofLayer = rootForeignParent(popup) && LayerSurface::from(rootForeignParent(popup));
    const bool nests = m_popupGrabs.empty()
                           ? ofLayer || (m_focused && root && root->xdgSurface() &&
                                         root->xdgSurface()->surface() == m_focused->toplevel->surface())
                           : popup->parent() == m_popupGrabs.back()->xdgSurface();
    if (!fromInput || !nests) {
        log::debug("desktop", "refusing popup grab (input: {}, nesting: {})", fromInput, nests);
        popup->dismiss();
        return;
    }
    m_popupGrabs.push_back(popup);
    m_server.seat().setKeyboardFocus(surface);
}

void Desktop::popupGone(XdgPopup* popup)
{
    std::erase(m_popups, popup);
    const bool wasTop = !m_popupGrabs.empty() && m_popupGrabs.back() == popup;
    if (std::erase(m_popupGrabs, popup) && wasTop)
        restoreKeyboardFocus();
}

wl_client* Desktop::popupGrabClient() const
{
    if (m_popupGrabs.empty())
        return nullptr;
    const XdgSurface* xdg = m_popupGrabs.back()->xdgSurface();
    return xdg && xdg->surface() ? xdg->surface()->client() : nullptr;
}

void Desktop::dismissPopups()
{
    auto grabs = std::exchange(m_popupGrabs, {});
    for (auto it = grabs.rbegin(); it != grabs.rend(); ++it) // topmost first
        (*it)->dismiss();
    restoreKeyboardFocus();
}

void Desktop::restoreKeyboardFocus()
{
    Surface* surface = nullptr;
    if (!m_popupGrabs.empty()) {
        if (auto* xdg = m_popupGrabs.back()->xdgSurface())
            surface = xdg->surface();
    } else if (m_layerFocus && m_layerFocus->surface()) {
        surface = m_layerFocus->surface();
    } else if (m_focused) {
        surface = m_focused->toplevel->surface();
    }
    m_server.seat().setKeyboardFocus(surface);
}

void Desktop::focus(Window* window)
{
    // Focusing anything else ends menus.
    if (!m_popupGrabs.empty() && (!window || !m_focused || window != m_focused))
        dismissPopups();
    if (window && window->minimized()) {
        unminimize(*window); // comes back here
        return;
    }
    if (window) {
        // Raise: move to the top of the stack.
        auto it = std::ranges::find_if(m_windows, [window](const auto& w) { return w.get() == window; });
        if (it == m_windows.end())
            return;
        std::rotate(it, it + 1, m_windows.end());
        window->toplevel->raised();
    }
    // Picking a window takes the keyboard back from an on-demand layer surface
    // (never from an exclusive one: a lock screen stays in charge).
    const bool fromLayer = window && m_layerFocus && m_layerFocus->state().keyboard == LayerSurface::Keyboard::OnDemand;
    if (fromLayer)
        m_layerFocus = nullptr;
    if (window == m_focused) {
        if (fromLayer)
            restoreKeyboardFocus();
        return;
    }

    if (m_focused)
        m_focused->toplevel->setActivated(false);
    m_focused = window;
    if (window)
        window->toplevel->setActivated(true);
    restoreKeyboardFocus();
    focusChanged.emit(window);
}

Surface* Desktop::rootSurface(Surface* surface)
{
    while (surface) {
        auto* sub = surface->asSubsurface();
        if (!sub || !sub->parent())
            return surface;
        surface = sub->parent();
    }
    return nullptr;
}

std::optional<Point> Desktop::originOf(Surface* surface) const
{
    Point offset;
    Surface* s = surface;
    while (auto* sub = s ? s->asSubsurface() : nullptr) {
        if (!sub->parent())
            return std::nullopt;
        offset = offset + sub->position();
        s = sub->parent();
    }
    if (!s)
        return std::nullopt;
    if (const Window* w = windowForSurface(s))
        return w->position - w->toplevel->geometry().position() + offset;
    if (auto* layer = LayerSurface::from(s)) {
        if (auto p = layerPosition(layer))
            return *p + offset;
        return std::nullopt;
    }
    for (const auto& u : m_unmanaged) {
        if (u->surface == s)
            return u->position + offset;
    }
    if (const auto* xdg = dynamic_cast<const XdgSurface*>(s->role())) {
        if (auto origin = surfaceOrigin(xdg))
            return *origin + offset;
    }
    return std::nullopt;
}

Desktop::Window* Desktop::windowOf(Surface* surface) const
{
    Surface* root = rootSurface(surface);
    if (Window* w = windowForSurface(root))
        return w;
    const auto* xdg = root ? dynamic_cast<const XdgSurface*>(root->role()) : nullptr;
    if (xdg && xdg->popup()) {
        const XdgToplevel* top = rootToplevel(xdg->popup());
        return top && top->xdgSurface() ? windowForSurface(top->xdgSurface()->surface()) : nullptr;
    }
    return nullptr;
}

bool Desktop::hitTree(Surface* surface, Point origin, PointF point, Hit& hit)
{
    const auto& stack = surface->stack();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) { // topmost first
        Surface* entry = *it;
        if (entry == surface) {
            if (!surface->hasContent())
                continue;
            const PointF local = point - origin;
            const Size size = surface->size();
            const Point cell{std::int32_t(std::floor(local.x)), std::int32_t(std::floor(local.y))};
            if (local.x >= 0 && local.y >= 0 && local.x < size.width && local.y < size.height &&
                surface->state().input.contains(cell)) {
                hit = {surface, local};
                return true;
            }
        } else if (auto* sub = entry->asSubsurface(); sub && entry->hasContent()) {
            if (hitTree(entry, origin + sub->position(), point, hit))
                return true;
        }
    }
    return false;
}

Desktop::Hit Desktop::surfaceAt(PointF point) const
{
    const auto items = renderList();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        // Styled windows are cut to their geometry: client-side shadows and
        // resize margins are neither drawn nor hit (our frame does that).
        if (it->window) {
            const Frame f = frameOf(*it->window);
            if (f.styled && !f.content.contains(point))
                continue;
        }
        Hit hit;
        if (hitTree(it->surface, it->origin, point, hit))
            return hit;
    }
    return {};
}

} // namespace dragon
