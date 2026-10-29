#pragma once

#include "desktop/Toplevel.h"

#include <vector>

namespace dragon {

class XdgToplevel;

// Toplevel over an xdg_toplevel.
class XdgWindow final : public Toplevel {
public:
    explicit XdgWindow(XdgToplevel& toplevel);
    ~XdgWindow() override;

    XdgToplevel& xdg() const { return m_toplevel; }

    Surface* surface() const override;
    Rect geometry() const override;
    std::string title() const override;
    std::string appId() const override;
    Size minSize() const override;
    Size maxSize() const override;
    bool maximized() const override;
    bool fullscreen() const override;

    void setActivated(bool on) override;
    void setSize(Size size) override;
    void setMaximized(bool on) override;
    void setFullscreen(bool on) override;
    void setResizing(bool on) override;
    void setBounds(Size bounds) override;
    void close() override;

private:
    XdgToplevel& m_toplevel;
    std::vector<Connection> m_connections;
    Connection m_surfaceCommitted;
};

} // namespace dragon
