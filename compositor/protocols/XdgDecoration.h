#pragma once

#include "core/Signal.h"
#include "dragon-protocols/xdg-decoration-unstable-v1.h"

#include <memory>
#include <optional>

namespace dragon {

class XdgToplevel;

// zxdg_toplevel_decoration_v1: a client asking who draws its title bar. The
// desktop decides (XdgToplevel::setServerSideDecoration); the client's own
// preference is kept for the desktop to consult.
class XdgToplevelDecoration final : public proto::ZxdgToplevelDecorationV1 {
public:
    XdgToplevelDecoration(wl_client* client, std::uint32_t version, std::uint32_t id, XdgToplevel* toplevel);
    ~XdgToplevelDecoration() override;

    // What the client asked for with set_mode (nullopt: no preference).
    std::optional<Mode> requested() const { return m_requested; }

protected:
    void handleSetMode(Mode mode) override;
    void handleUnsetMode() override;

private:
    XdgToplevel* m_toplevel; // null once the toplevel is gone
    std::optional<Mode> m_requested;
    Connection m_toplevelGone;
};

class Global;
// The zxdg_decoration_manager_v1 global.
std::unique_ptr<Global> createXdgDecorationGlobal(wl_display* display);

} // namespace dragon
