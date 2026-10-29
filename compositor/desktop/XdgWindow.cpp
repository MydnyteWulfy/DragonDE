#include "desktop/XdgWindow.h"

#include "protocols/Compositor.h"
#include "protocols/XdgShell.h"

namespace dragon {

XdgWindow::XdgWindow(XdgToplevel& toplevel) : m_toplevel(toplevel)
{
    XdgSurface* xdg = toplevel.xdgSurface();
    m_connections.push_back(xdg->mapped.connect([this] {
        if (Surface* s = surface())
            m_surfaceCommitted = s->committed.connect([this] { committed.emit(); });
        mapped.emit();
    }));
    m_connections.push_back(xdg->unmapped.connect([this] {
        m_surfaceCommitted.disconnect();
        unmapped.emit();
    }));
    m_connections.push_back(toplevel.moveRequested.connect([this](std::uint32_t serial) { moveRequested.emit(serial); }));
    m_connections.push_back(toplevel.resizeRequested.connect([this](std::uint32_t serial, proto::XdgToplevelResizeEdge e) {
        resizeRequested.emit(serial, std::uint32_t(e));
    }));
    m_connections.push_back(toplevel.maximizeRequested.connect([this](bool on) { maximizeRequested.emit(on); }));
    m_connections.push_back(toplevel.fullscreenRequested.connect([this](bool on) { fullscreenRequested.emit(on); }));
    // ~XdgToplevel unmaps first (roleDestroyed), then `destroyed` fires.
    m_connections.push_back(toplevel.destroyed.connect([this] { destroyed.emit(); }));
}

XdgWindow::~XdgWindow() = default;

Surface* XdgWindow::surface() const
{
    XdgSurface* xdg = m_toplevel.xdgSurface();
    return xdg ? xdg->surface() : nullptr;
}

Rect XdgWindow::geometry() const
{
    XdgSurface* xdg = m_toplevel.xdgSurface();
    return xdg ? xdg->geometry() : Rect{};
}

std::string XdgWindow::title() const { return m_toplevel.title(); }
std::string XdgWindow::appId() const { return m_toplevel.appId(); }
Size XdgWindow::minSize() const { return m_toplevel.minSize(); }
Size XdgWindow::maxSize() const { return m_toplevel.maxSize(); }
bool XdgWindow::maximized() const { return m_toplevel.pendingConfigure().maximized; }
bool XdgWindow::fullscreen() const { return m_toplevel.pendingConfigure().fullscreen; }

void XdgWindow::setActivated(bool on) { m_toplevel.setActivated(on); }
void XdgWindow::setSize(Size size) { m_toplevel.setSize(size); }
void XdgWindow::setMaximized(bool on) { m_toplevel.setMaximized(on); }
void XdgWindow::setFullscreen(bool on) { m_toplevel.setFullscreen(on); }
void XdgWindow::setResizing(bool on) { m_toplevel.setResizing(on); }
void XdgWindow::setBounds(Size bounds) { m_toplevel.setBounds(bounds); }
void XdgWindow::close() { m_toplevel.requestClose(); }

} // namespace dragon
