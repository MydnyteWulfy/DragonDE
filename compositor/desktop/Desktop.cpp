#include "desktop/Desktop.h"

#include "Server.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <cmath>

namespace dragon {

// Signal connections for one toplevel or popup, dropped when it's destroyed.
struct Desktop::Tracked {
    std::vector<Connection> connections;
};

Desktop::Desktop(Server& server) : m_server(server)
{
    m_toplevelCreated = server.toplevelCreated.connect([this](XdgToplevel* t) { trackToplevel(t); });
    m_popupCreated = server.popupCreated.connect([this](XdgPopup* p) { trackPopup(p); });
}

Desktop::~Desktop() = default;

Desktop::Window* Desktop::windowFor(const XdgToplevel* toplevel) const
{
    auto it = std::ranges::find_if(m_windows, [toplevel](const auto& w) { return w->toplevel == toplevel; });
    return it == m_windows.end() ? nullptr : it->get();
}

void Desktop::trackToplevel(XdgToplevel* toplevel)
{
    auto tracked = std::make_unique<Tracked>();
    XdgSurface* xdg = toplevel->xdgSurface();
    tracked->connections.push_back(xdg->mapped.connect([this, toplevel] { mapWindow(toplevel); }));
    tracked->connections.push_back(xdg->unmapped.connect([this, toplevel] { unmapWindow(toplevel); }));
    // Before mapping there's no Window yet: answer in the first configure.
    tracked->connections.push_back(toplevel->maximizeRequested.connect([this, toplevel](bool on) {
        if (Window* w = windowFor(toplevel))
            setMaximized(*w, on);
        else {
            toplevel->setMaximized(on);
            toplevel->setSize(on ? m_area.size() : Size{});
        }
    }));
    tracked->connections.push_back(toplevel->fullscreenRequested.connect([this, toplevel](bool on) {
        if (Window* w = windowFor(toplevel))
            setFullscreen(*w, on);
        else {
            toplevel->setFullscreen(on);
            toplevel->setSize(on ? m_area.size() : Size{});
        }
    }));
    tracked->connections.push_back(toplevel->destroyed.connect([this, toplevel] {
        unmapWindow(toplevel); // no-op if already unmapped
        m_tracked.erase(toplevel);
    }));
    m_tracked.emplace(toplevel, std::move(tracked));
}

void Desktop::trackPopup(XdgPopup* popup)
{
    auto tracked = std::make_unique<Tracked>();
    XdgSurface* xdg = popup->xdgSurface();
    tracked->connections.push_back(xdg->mapped.connect([this, popup] { m_popups.push_back(popup); }));
    tracked->connections.push_back(xdg->unmapped.connect([this, popup] { std::erase(m_popups, popup); }));
    tracked->connections.push_back(popup->destroyed.connect([this, popup] {
        std::erase(m_popups, popup);
        m_tracked.erase(popup);
    }));
    m_tracked.emplace(popup, std::move(tracked));
}

void Desktop::mapWindow(XdgToplevel* toplevel)
{
    const Rect geometry = toplevel->xdgSurface()->geometry();

    // Centered, cascading so new windows don't hide each other exactly.
    const std::int32_t offset = m_cascade * 32;
    m_cascade = (m_cascade + 1) % 8;
    Point pos{m_area.x + (m_area.width - geometry.width) / 2 + offset,
              m_area.y + (m_area.height - geometry.height) / 2 + offset};
    pos.x = std::max(pos.x, m_area.x);
    pos.y = std::max(pos.y, m_area.y);
    const auto& configured = toplevel->pendingConfigure();
    if (configured.maximized || configured.fullscreen)
        pos = m_area.position(); // started maximized: fill the area

    m_windows.push_back(std::make_unique<Window>(Window{.toplevel = toplevel, .position = pos}));
    Window* window = m_windows.back().get();
    window->connections.push_back(
        toplevel->moveRequested.connect([this, window](std::uint32_t serial) { moveRequested.emit(window, serial); }));
    window->connections.push_back(toplevel->resizeRequested.connect(
        [this, window](std::uint32_t serial, proto::XdgToplevelResizeEdge edges) {
            resizeRequested.emit(window, serial, std::uint32_t(edges));
        }));
    if (Surface* surface = toplevel->xdgSurface()->surface())
        window->connections.push_back(surface->committed.connect([this, window] { committed(*window); }));
    log::debug("desktop", "mapped '{}' ({}) {}x{} at {},{}", toplevel->title(), toplevel->appId(), geometry.width,
               geometry.height, pos.x, pos.y);
    windowMapped.emit(window);
    focus(window); // new windows get focus
}

void Desktop::unmapWindow(XdgToplevel* toplevel)
{
    auto it = std::ranges::find_if(m_windows, [toplevel](const auto& w) { return w->toplevel == toplevel; });
    if (it == m_windows.end())
        return;
    auto window = std::move(*it);
    m_windows.erase(it);
    log::debug("desktop", "unmapped '{}'", toplevel->title());
    windowUnmapped.emit(window.get());
    if (m_focused == window.get()) {
        m_focused = nullptr;
        focus(m_windows.empty() ? nullptr : m_windows.back().get()); // next one down
    }
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
    if (const XdgToplevel* toplevel = xdg->toplevel()) {
        const Window* w = windowFor(toplevel);
        if (!w)
            return std::nullopt;
        return w->position - geometryOffset;
    }
    if (const XdgPopup* popup = xdg->popup()) {
        const XdgSurface* parent = popup->parent();
        if (!parent)
            return std::nullopt;
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
    std::vector<RenderItem> items;
    for (const auto& w : m_windows) {
        XdgSurface* xdg = w->toplevel->xdgSurface();
        if (!xdg || !xdg->surface())
            continue;
        if (auto origin = surfaceOrigin(xdg))
            items.push_back({xdg->surface(), *origin});

        for (XdgPopup* popup : m_popups) {
            if (rootToplevel(popup) != w->toplevel || !popup->xdgSurface() || !popup->xdgSurface()->surface())
                continue;
            if (auto origin = surfaceOrigin(popup->xdgSurface()))
                items.push_back({popup->xdgSurface()->surface(), *origin});
        }
    }
    return items;
}

Rect Desktop::geometryOf(const Window& window) const
{
    const XdgSurface* xdg = window.toplevel->xdgSurface();
    const Size size = xdg ? xdg->geometry().size() : Size{};
    return {window.position.x, window.position.y, size.width, size.height};
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
    if (edges & std::uint32_t(proto::XdgToplevelResizeEdge::Left))
        window.position.x = window.resizeStart.x + window.resizeStart.width - g.width;
    if (edges & std::uint32_t(proto::XdgToplevelResizeEdge::Top))
        window.position.y = window.resizeStart.y + window.resizeStart.height - g.height;
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
    XdgToplevel* tl = window.toplevel;
    if (fullscreen)
        tl->setFullscreen(on);
    else
        tl->setMaximized(on);

    const auto& cfg = tl->pendingConfigure();
    const bool filling = cfg.maximized || cfg.fullscreen;
    if (filling) {
        if (!window.restore)
            window.restore = geometryOf(window);
        // No panels yet: maximized and fullscreen both fill the whole area.
        tl->setSize(m_area.size());
        window.position = m_area.position();
    } else if (window.restore) {
        tl->setSize(window.restore->size());
        window.position = window.restore->position();
        window.restore.reset();
    }
}

void Desktop::focus(Window* window)
{
    if (window) {
        // Raise: move to the top of the stack.
        auto it = std::ranges::find_if(m_windows, [window](const auto& w) { return w.get() == window; });
        if (it == m_windows.end())
            return;
        std::rotate(it, it + 1, m_windows.end());
    }
    if (window == m_focused)
        return;

    if (m_focused)
        m_focused->toplevel->setActivated(false);
    m_focused = window;
    Surface* surface = nullptr;
    if (window) {
        window->toplevel->setActivated(true);
        if (auto* xdg = window->toplevel->xdgSurface())
            surface = xdg->surface();
    }
    m_server.seat().setKeyboardFocus(surface);
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
    const auto* xdg = dynamic_cast<const XdgSurface*>(s->role());
    if (!xdg)
        return std::nullopt;
    auto origin = surfaceOrigin(xdg);
    if (!origin)
        return std::nullopt;
    return *origin + offset;
}

Desktop::Window* Desktop::windowOf(Surface* surface) const
{
    Surface* root = rootSurface(surface);
    const auto* xdg = root ? dynamic_cast<const XdgSurface*>(root->role()) : nullptr;
    if (!xdg)
        return nullptr;
    if (xdg->toplevel())
        return windowFor(xdg->toplevel());
    if (xdg->popup())
        return windowFor(rootToplevel(xdg->popup()));
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
        Hit hit;
        if (hitTree(it->surface, it->origin, point, hit))
            return hit;
    }
    return {};
}

} // namespace dragon
