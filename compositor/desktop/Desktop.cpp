#include "desktop/Desktop.h"

#include "Server.h"
#include "core/Log.h"
#include "protocols/XdgShell.h"

#include <algorithm>

namespace dragon {

// Signal connections for one toplevel or popup, dropped when it's destroyed.
struct Desktop::Tracked {
    std::vector<Connection> connections;
};

Desktop::Desktop(Server& server)
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

    m_windows.push_back(std::make_unique<Window>(Window{toplevel, pos}));
    log::debug("desktop", "mapped '{}' ({}) {}x{} at {},{}", toplevel->title(), toplevel->appId(), geometry.width,
               geometry.height, pos.x, pos.y);
    windowMapped.emit(m_windows.back().get());
}

void Desktop::unmapWindow(XdgToplevel* toplevel)
{
    auto it = std::ranges::find_if(m_windows, [toplevel](const auto& w) { return w->toplevel == toplevel; });
    if (it == m_windows.end())
        return;
    auto window = std::move(*it);
    m_windows.erase(it);
    windowUnmapped.emit(window.get());
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

} // namespace dragon
